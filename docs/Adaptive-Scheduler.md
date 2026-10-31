# Adaptive Scheduler

The scheduler moves the mesh from **static even channel-slicing** to **yield-driven
adaptive acquisition**: scan the channels that actually produce APs, don't duplicate work,
and keep the ESP-NOW RX path deadline-bound. It lives in
[`scheduler/`](https://github.com/olosnoks/warhog/tree/main/scheduler) as standalone,
host-testable headers (no Arduino/Wi-Fi deps where avoidable; `now` is passed in), wired
into every node firmware's scan loop.

Full design: [`scheduler/DESIGN.md`](https://github.com/olosnoks/warhog/blob/main/scheduler/DESIGN.md).
Current state & gotchas: [`scheduler/CLAUDE.md`](https://github.com/olosnoks/warhog/blob/main/scheduler/CLAUDE.md).

## Feature flags
Each phase is a compile-time flag. **On by default** in released builds; set to `0` to get
byte-identical legacy behavior. `tests/test_all.cpp` asserts every flag's OFF default, so a
flag's ON path gets its own test TU (`test_dwell_on.cpp`, `test_pair_on.cpp`).

| Flag | Phase | What it does |
|---|---|---|
| `PIG_FEAT_METRICS` | 0 | 1 Hz `PIGM,` metrics line (warhog_fork) |
| `PIG_FEAT_DEDUP` | 1 | worker-side BSSID dedup |
| `PIG_FEAT_ADAPTIVE_DWELL` | 2 | two-pass sweep: fast active, then yield-weighted passive revisits; DFS passive |
| `PIG_FEAT_DYNAMIC_ASSIGN` | 3 | Core yield-weighted channel assignment (`PIG_MSG_ADMIN_EXT`) |
| `PIG_FEAT_CORE_ISO` | 4 (Steps 1+5) | SD logging **and** the ESP-NOW record drain each on a core-0 task + SPI-bus mutex |
| `PIG_FEAT_PAIR_SCHED` | 5 | per-channel dwell table + complementary antenna-pair assignment + overlap metric |

> Precedence: `PIG_FEAT_PAIR_SCHED` overrides Phases 2/3 at the `pig_scan_plan()` /
> `coreAssignGroup()` branch points — it is the current **default** scan model.

---
## Phase 1 — worker dedup (`pig_dedup.h`, `bssid_cache.h`)
Before a node re-sends a record, `pigDedupSkip()` consults a rolling 512-slot TTL cache
(`BssidCache`, ~8 KB, open-addressed, approx-LRU) that classifies each sighting
**NEW / CHANGED / REPEAT / SUPPRESS**. Suppressed repeats aren't transmitted. Measured
**~29 % airtime saved stationary** on hardware (more in motion). A NEW/unique BSSID is
never dropped before a DELTA under backpressure.

## Phase 2 — adaptive dwell + two-pass sweep (`pig_dwell.h`)
`pig_scan_plan(ch, pass, yield)` returns `{passive, dwell_ms}`:
- **Fast pass:** active, short dwell (`PIG_DWELL_FAST_MS` = 50 ms), DFS forced passive.
- **Revisit pass:** passive, `dwell = floor(110) + yield×20`, clamped to 300 ms.

The `PigSweep` FSM drives a node's whole cycle: a fast active pass scores every assigned
channel by yield, then a passive revisit pass dwells deeper on the top ~half of productive
channels (cap 8). API: `pig_sweep_build()` → `pig_sweep_begin()` → `pig_sweep_next()` →
`pig_sweep_report(yield)`; `pig_sweep_invalidate()` on any assignment change rebuilds the
cycle. Unscannable channels (5 GHz on 2.4-only silicon) are dropped at build time.

## Phase 3 — dynamic yield-weighted assignment (`yield_sched.h`)
The Core tracks per-channel yield from received records (EWMA via `sched_on_yield`) and
orders each dual-band node's 5 GHz channels by measured yield (`sched_rank`), front-loading
the productive ones. **Band-aware:** 2.4-only nodes still get `{1,6,11}`. A 3-gate
hysteresis prevents assignment thrash. Delivered via `PIG_MSG_ADMIN_EXT` (0x23).

## Phase 4 — core isolation (`PIG_FEAT_CORE_ISO`, `SpiBusLock.h`, `pig_queues.h`)
Keeps the deadline-bound RX path off the heavy UI/web loop on dual-core S3 Cores.
Acceptance target (DESIGN §13): **0 RX deadline misses, RX jitter < 5 ms, at ≥12 nodes.**

- **Step 1 — SD isolation:** the per-record WiGLE SD write runs on a FreeRTOS task pinned
  to **core 0** (`tStore`), fed by a 48-deep lossy `xQueue`; the loop keeps draining
  ESP-NOW. `tStore`, the UI's `present()`, and upload-time file moves (`moveToFolder`,
  run on core 1 via `WigleUpload`/`WebUI`, guarded by a `SpiBusGuard` RAII scope) all
  take the `SpiBusLock.h` recursive mutex so they can't drive the bus at once.
  **Board reality:** the mutex was written for the ILI9341-SPI-TFT CYD variant where SD
  and the TFT literally share pins 10–13. On the two current build envs they don't — the
  **freenove** uses SDIO SD (separate pins) and the **guition W550** an RGB (parallel) TFT
  — so the only real same-bus contenders there are `tStore` and `moveToFolder`, both SPI-SD
  writers on the guition. `PIG_FEAT_CORE_ISO` ships ON for both CYD envs, so the lock is live.
- **Step 5 — RX-drain isolation:** the `coreTextBuf` drain (parse + SD-enqueue) moves onto
  a second core-0 task (`tRxDrain`); the phone-app mirror + UI telemetry are handed back to
  the loop via a `coreMirrorBuf` SPSC ring so NimBLE/UI state stays single-core.
  **Measured result (4-node bench):** RX-servicing jitter dropped from ~100–220 ms (loop)
  to **2–3 ms with 0 ticks over 5 ms**, with 0 dropped records — the jitter target met,
  while the heavy loop runs undisturbed.
- Steps 2–4 (separate UI/GNSS/upload tasks) turned out **unnecessary**: Steps 1+5 already
  decouple the RT path.
- **RAM note:** Step 5 only fit after reclaiming ~32 KB of internal RAM — the Marauder
  PCAP double-buffer is now allocated lazily (only during capture) instead of reserved as
  static `.bss`. Internal heap, not threading, was the binding constraint on the S3 Core.
- Instrumentation: the `[CORE-ISO]` line (`rt_us`, `rx_drop`, `txtq_max`, `loop_us`).
- On the single-core C3 there's no affinity → priority-only fallback.

## Phase 5 — WarHog dual-node pair scheduler (`PIG_FEAT_PAIR_SCHED`)
For rigs where **two nodes share one antenna** (6 antennas × 2 C5 = 12 nodes). The goal:
the second receiver should *widen* instantaneous coverage, not duplicate the first.

- **Per-channel dwell table** (`pig_pair_plan()` in `pig_dwell.h`), from field data:
  70 ms on 1/6/11, 60 ms on the high-value non-DFS 5 GHz (36/40/44/48/149/153/157/161),
  30 ms on low 2.4 + 165, 120 ms passive on DFS/12–14. The sweep becomes a single
  per-channel-dwell pass (no yield-revisit); re-cycling the short list gives the revisit
  cadence (~650–800 ms target on high-value channels).
- **Complementary antenna-pair assignment** (`coreAssignGroup`, S3 Core): each sub-cluster
  group is one antenna; its members take alternating **A/B** roles with disjoint channel
  lists (`PIG_PAIR_LIST_A`/`_B`) — e.g. A=`6,157,44,36,153,8,2,4,10,165`,
  B=`1,11,161,48,149,40,9,3,5,7`. High-value 2.4 is split (A=6, B=1+11). Lists are
  **phase-offset per group** so the six pairs don't scan in lockstep, and each node gets a
  rotating **complementary DFS** pair.
- **Pair-overlap metric:** the `[PAIR] grp=g overlap both=B either=E pct=P%` line (every
  5 s) measures how often a group's two nodes see the same BSSID within a second — the
  redundancy the metric exists to drive down. Bench read 0–16 % with complementary lists.
- **Status:** default-on, bench-validated on an S3 Core + 4 C5 nodes (assignment, metric,
  coexistence with Step-5). Field-only: multi-group phase offset (needs ≥2 antennas),
  precise DFS interleave cadence, and node-side per-channel dwell end-to-end.

---
## Where each piece lives
| File | Role |
|---|---|
| `pig_proto.h` | wire structs, `PIG_MSG_ADMIN_EXT`, CRC16 |
| `bssid_cache.h` | TTL dedup cache |
| `pig_dedup.h` | Phase 1 shim |
| `pig_dwell.h` | Phase 2 dwell + `PigSweep` FSM; Phase 5 per-channel table + role lists |
| `yield_sched.h` | Phase 3 scoring + `sched_rank` + hysteresis |
| `pig_queues.h` / `lossy_ring.h` | Phase 4 cross-core queues + lossy ring |
| `pig_metrics.h` / `pig_replay.h` | Phase 0 metrics line + A/B replay harness |

## Benchmarking
Use the replay harness (`tests/replay.cpp`, `pig_replay.h`) to A/B transmit models on an
identical captured scan corpus rather than live RF. The regression guard: any build that
cuts single-node unique/min > 5 % or introduces an RX deadline miss fails and reverts.
