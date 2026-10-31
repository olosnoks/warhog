# Context for Claude sessions working in scheduler/

This folder is the **yield-driven mesh scheduler**: the move from static even
channel-slicing to yield-driven acquisition. Full technical design in
[`DESIGN.md`](./DESIGN.md); Phase-4 plan in [`PHASE4_PLAN.md`](./PHASE4_PLAN.md).
[`HANDOFF.md`](./HANDOFF.md) is the original design chat, kept as an **archive** —
it predates all firmware wiring, so don't read it as current state.

## Where things stand
Phases 1–5 are merged to `main` and **ON by default** in the released builds (each
board's `platformio.ini` `build_flags` set `-DPIG_FEAT_*=1`). As of 2026-10-24 the
pipeline is **bench-validated on an S3 CYD Core + up to 4 C5 nodes** (dedup, dynamic
assignment, SD isolation, Step-5 RX-drain task, and the Phase-5 pair scheduler + overlap
metric). **Phase 5 changes the DEFAULT scan model** from yield-driven dynamic assignment
to the friend's deterministic per-channel/antenna-pair scheduler (`PIG_FEAT_PAIR_SCHED`
overrides Phase-2/3 at the `pig_scan_plan`/`coreAssignGroup` branch points). Outstanding:
formal **≥12-node scale sign-off** on a larger cluster (the friend's WarHog rig), plus
Phase-5 field-test refinements (see What's left). Day-to-day work is fine-tuning.

| Flag | Phase | What it does | Boards |
|------|-------|--------------|--------|
| `PIG_FEAT_METRICS` | 0 | 1 Hz `PIGM,` metrics line | warhog_fork (hard-on in `PigMetricsHook.h`) |
| `PIG_FEAT_DEDUP` | 1 | worker BSSID dedup (~29% airtime saved, validated) | all mesh nodes |
| `PIG_FEAT_ADAPTIVE_DWELL` | 2 | two-pass sweep: 50 ms active fast pass, then yield-weighted passive revisits; DFS 5 GHz passive | all mesh nodes |
| `PIG_FEAT_DYNAMIC_ASSIGN` | 3 | yield-weighted channel assignment (`PIG_MSG_ADMIN_EXT`) | Core: warhog_fork, CYD, W550 |
| `PIG_FEAT_CORE_ISO` | 4 Steps 1+5 | SD logging **and** the ESP-NOW record drain each get their own core-0 FreeRTOS task + SPI-bus mutex; `[CORE-ISO]` line reports `rt_us`/`rx_drop`/`txtq_max`/`loop_us` | warhog_fork, CYD, W550 |
| `PIG_FEAT_PAIR_SCHED` | 5 | WarHog dual-node antenna-pair scheduler: explicit per-channel dwell table (70/60/30/120 ms) + single-pass sweep (nodes, via `pig_dwell.h`); Core gives each sub-cluster group's members complementary A/B channel lists + phase offset + rotating DFS (`coreAssignGroup`, S3 only); `[PAIR]` line reports per-group overlap %. **ON by default.** | all mesh nodes (dwell); S3 Core (pair assign + metric) |

**Do NOT change the `#define … 0` defaults in `pig_*.h`** — `tests/test_all.cpp`
asserts the OFF-default legacy behavior. A flag's ON path gets its own test TU
instead (see `tests/test_dwell_on.cpp`).

## How the firmwares use the Phase-2 sweep
Each node firmware holds one `static PigSweep nodeSweep;` and drives it from its
scan tick (`nodeDoScanTick()` / `scanTick()`):

1. `!pig_sweep_valid(&nodeSweep)` → build the channel list with
   `pig_sweep_build()` (explicit Phase-3/Biscuit list if there is one, else the
   contiguous table slice; unscannable channels dropped) and `pig_sweep_begin()`.
2. `pig_sweep_next()` yields the next channel + `PigScanPlan`; `false` means the
   sweep is done → ch-6 admin window.
3. After each async scan completes, `pig_sweep_report()` records that channel's
   yield (everything Phase-1 dedup did **not** suppress) and advances.

Every site that changes a node's assignment calls `pig_sweep_invalidate()` so the
next tick rebuilds. A zero-initialized `PigSweep` is already invalid, so a static
one is born needing a build.

## Build / test
```bash
# Host unit tests — 175 checks, 3 TUs + the replay smoke test.
./scheduler/tests/run.sh
# No host g++ on Windows; run it under WSL:
wsl bash -c "cd /mnt/c/Users/skons/Desktop/warhog/scheduler/tests && bash run.sh"

# Firmware (Windows, Git Bash):
/c/Users/skons/.platformio/penv/Scripts/pio.exe run -d "<dir>" -e "<env>"
# turn a flag off for a one-off build without editing the ini:
PLATFORMIO_BUILD_FLAGS="-DPIG_FEAT_CORE_ISO=0" pio.exe run -d <dir> -e <env>
```

CI matrix (`.github/workflows/build-and-deploy.yml`): `xiao_c5` (`warhog_fork`),
`freenove-s3-cyd-ham-ui4` + `guition-jc8048w550` (`WarHog_Freenove_S3_CYD_HAM_UI4`),
`esp32c3_oled` (`c3_node`), `warhognode_c5` (`WarHogNode`). The `waveshare_c6` solo
wardriver is not a mesh node and has no scheduler.

`TDongleC5_WarHog/` is an Arduino sketch, **not in CI** — Arduino has no `-I`, so it
keeps **copies** of `pig_proto.h`/`pig_dedup.h`/`pig_dwell.h`. Keep those copies
byte-identical to this folder's, and remember changes there get no build gate.

## Hard invariants (do not break)
- Record packets stay **212 B** (Biscuit Pro drops anything smaller).
- **DFS 5 GHz (ch 52–144) is passive-only** — compliance, not a tunable.
- Never drop a NEW/unique BSSID before a DELTA under backpressure.
- New message types stay **≥ 0x20** so legacy Cores ignore them.
- `pig_proto.h` must be `#include`d **unconditionally** in each firmware — the
  `PIG_MSG_ADMIN_EXT` worker parse needs it even with `PIG_FEAT_DEDUP` off.

## What's left (signed off 2026-10-24 — Phases 1–4 bench-validated)
Phases 1–4 are implemented, ON by default, and validated on an S3 CYD Core + 4 C5 nodes.
What remains is sign-off at scale plus one old bug — not new phases.

1. **Formal ≥12-node scale sign-off** (the DESIGN §13 acceptance node count). At 4 nodes
   the Step-5 RX-drain task holds `rt_us max` 2–3 ms with `gt5ms=0` (< 5 ms jitter target
   MET) and `rx_drop=0`; confirm that holds at ~12 nodes on a larger cluster. Watch the
   `[CORE-ISO]` line: `rt_us gt5ms` should stay 0, `rx_drop`/`txtq_max`/`drops` low. The
   heavy main loop still shows `loop_us max` ~100–220 ms — that's expected and harmless now
   (RX is decoupled onto core 0); don't chase it. Disable a suspect board with
   `-DPIG_FEAT_CORE_ISO=0`.
2. **CYD node-mode reboot** — the CYD crashes when switched to node mode; needs a
   backtrace over a stable serial link (unresolved, pre-existing).
3. **Fine-tuning** — `SchedCfg` constants, dwell/revisit counts, yield EWMA, task stack
   sizes (`uxTaskGetStackHighWaterMark`) are first-guess defaults; tune from field logs.

**Phase 4 note:** the plan's Steps 2–4 (separate UI/GNSS/upload tasks) were NOT needed —
Step 1 (SD) + Step 5 (RX drain) on core 0 already decouple the deadline-bound path from the
heavy core-1 loop and meet the jitter target, so UI/GNSS/upload stay in the loop by design.
Step 5 only fit after reclaiming ~32 KB internal RAM (lazy PCAP buffers in `MarauderTools.cpp`);
internal heap, not threading, was the binding constraint on the S3 Core.

## Gotchas (don't relearn these)
- Before committing warhog_fork, `rm -f "warhog_fork/WarHog.ino.cpp"`
  (Arduino build artifact; regenerates, never commit it).
- `COM13` = a Black Magic Probe (VID 1D50:6018), **not** an ESP. Real ESP32 native
  USB = 303A:1001. No-reset serial monitor: `$TEMP/readport.py` (dtr/rts=False).
- No `gh` CLI here. Open PRs / dispatch workflows via the GitHub REST API with the
  git credential token: `printf "protocol=https\nhost=github.com\n\n" | git credential fill`.
- Neither S3 board shares an SD+TFT SPI bus: Freenove = ILI9341 on SPI 10–13, SD on
  4-bit SDIO 38–48; Guition W550 = RGB panel, SD on SPI 10–13. `SpiBusLock.h` is
  really a cross-core SD lock (core-0 `tStore` vs core-1 moves/config saves); the
  lock in `ScreenUI::present()` is defensive only. Touch is I2C on both.
- The CYD's 64 GB SD card: 64 GB cards usually ship **exFAT** but the ESP32 SD stack
  wants **FAT32** — confirm the format before chasing an SD firmware bug.
