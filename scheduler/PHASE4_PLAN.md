# Phase 4 — FreeRTOS core isolation (implementation plan, bench-gated)

**Status (updated 2026-10-31):** SHIPPED. Steps 1 (SD store on a core-0 task) and 5
(RX-drain on a second core-0 task) are implemented behind `PIG_FEAT_CORE_ISO` and
**ship ON** in the CYD builds (`-DPIG_FEAT_CORE_ISO=1` in both env `build_flags`);
they were bench-validated at 4 nodes (RX jitter 2–3 ms, 0 drops). Steps 2–4 (separate
UI/GNSS/upload tasks) turned out **unnecessary** — Steps 1+5 already decouple the RT
path. What remains is ≥12-node scale confirmation (the user's real ceiling; see the
Adaptive-Scheduler wiki). The plan below is kept as the design record and the hazards
reference. Note: on the two current CYD build envs SD and TFT are **not** on one SPI
bus (freenove = SDIO SD, guition = RGB TFT), and no shared-bus board is planned. The
`SpiBusLock` mutex earns its keep as a cross-core SD lock (core-0 `tStore` vs core-1
file moves / config saves); its use in `ScreenUI::present()` is defensive only.

The original gating guidance still holds for any further task split: it is a threading
rewrite whose correctness is only knowable by load-testing a multi-node mesh; do it on
a bench, one task at a time, behind `PIG_FEAT_CORE_ISO`, each step validated first.

## Goal
Keep the RT path (ESP-NOW RX, scheduling, worker scan FSM) deadline-bound on core 0
while best-effort work (SD, GNSS, UI, upload) runs on core 1, joined by the bounded
queues — so SD/UI stalls can't cause ESP-NOW RX deadline misses. Acceptance (DESIGN
§13): **0 RX deadline misses, RX jitter p95 < 5 ms, at ≥12 nodes.**

## Tasks (DESIGN §7)
| Task       | Core | Prio | Stack | Role / queue |
|------------|------|------|-------|--------------|
| tEspNowRx  | 0    | 6    | 4 KB  | ESP-NOW RX cb → stamp → `qObs` (push) |
| tSched     | 0    | 5    | 6 KB  | yield scoring + assignment; drains `qCmd` source, emits `qCmd` |
| tScanFsm   | 0    | 4    | 4 KB  | (worker) async scan FSM → `qObs` (push) |
| tGNSS      | 1    | 4    | 3 KB  | UART GNSS parse → `qGps` (latest-wins) |
| tStore     | 1    | 3    | 6 KB  | drains `qObs` → SD (WiGLE CSV / PCAP) |
| tUI        | 1    | 2    | 4 KB  | OLED/TFT/PigWorld from snapshots |
| tUpload    | 1    | 1    | 6 KB  | WiGLE/Wardrift opportunistic upload |

Create with `xTaskCreatePinnedToCore(fn, name, stack, arg, prio, &handle, core)`.

## Queues (`pig_queues.h`)
- `qObs` = `PigObsQueue` (`LossyRing<pig_obs_t,128>`): RT→nonRT, **lossy** — refuses
  DELTA at ≥80%, overwrites oldest FULL only at 100% (unique-BSSID metric protected).
- `qGps` = `PigMailbox<GpsFix>`: latest-wins, length 1.
- `qCmd` = **reliable** (on-target: FreeRTOS `xQueue`, len 16; `ReliableRing` models it
  for host tests) — a command must never be dropped.

**Drop retention order under load (§11), keep top:** ADMIN/commands → heartbeats
(coalesce) → NEW/CHANGED fulls → DELTA/summaries → suppressed.

## Hazards (why blind is unsafe)
1. **Shared SPI bus (CYD): SD + TFT are on the same SPI.** Moving `tStore` (SD) to
   core 1 while `tUI` (TFT) draws on core 1 means two users of one bus — **a single
   `radioMutex`/`spiMutex` must guard every SD and TFT transaction** or the bus
   corrupts. Add the mutex BEFORE splitting SD off the main thread.
2. **Co-resident BLE/Marauder** also want the radio — the existing single-scan /
   `radioMutex` discipline (see memory: BLE radio ownership) must be preserved.
3. **Stack sizing** is a guess until measured — watch `uxTaskGetStackHighWaterMark`
   on each task on the bench and trim/grow.
4. **ESP32-C3 is single-core** → no affinity. Fallback = priority-only preemption
   (tEspNowRx prio 6 preempts all; SD writes in ≤512 B yielded chunks). Scan stays async.

## Suggested rollout order (each validated on a bench, flag-gated)
1. Add `spiMutex`; wrap all SD + TFT access. (No tasks yet — pure safety prep.)
2. Move **tStore** (SD logging) to a core-1 task fed by `qObs`; keep everything else in
   `loop()`. Validate: no RX deadline misses, no SD/TFT corruption, UI stays smooth.
3. Move **tUI** to core 1. 4. Move **tGNSS**/**tUpload**. 5. Finally split the RT trio
   (tEspNowRx/tSched/tScanFsm) onto core 0.
6. On C3, skip affinity; set priorities only.

Auto-revert gate: any build that introduces an RX deadline miss or cuts single-node
uniq/min > 5% fails and reverts (DESIGN §13).
