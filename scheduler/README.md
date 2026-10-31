# scheduler/ — yield-driven acquisition modules

Standalone, host-testable building blocks that move the WarHog mesh from static
even channel-slicing to **yield-driven adaptive scanning**. The modules are kept
isolated (no Arduino/WiFi deps where avoidable; `now` passed as a parameter) so
the logic unit-tests on a host before it ever touches a radio.

**Status: shipped and LIVE.** Every phase is wired into the firmware and the
feature flags are **on by default in the released flasher builds** (see
[Feature flags](#feature-flags) below). Each flag still compiles cleanly to the
legacy behavior when set to `0`, so a single phase can be disabled in isolation.

## Files

| File | Purpose |
|---|---|
| `pig_proto.h` | ESP-NOW wire structs (`pig_obs_t`, `pig_batch_t`, 212 B Biscuit-safe) + CRC16-CCITT + `pig_admin_ext_t` (explicit yield-ordered channel list). New message types `0x20–0x23` sit above the legacy `1..5` range so legacy Cores ignore them. |
| `bssid_cache.h` | Rolling TTL dedup cache (512 slots ≈ 8 KB, open-addressed, approx-LRU). Classifies each sighting NEW / CHANGED / REPEAT / SUPPRESS. |
| `pig_dedup.h` | **Phase 1.** Worker-side dedup shim (`pigDedupSkip()`), flag `PIG_FEAT_DEDUP`. Skips re-sending a BSSID the cache says is suppressed. ~29 % airtime saved stationary on hardware. |
| `pig_dwell.h` | **Phase 2.** Adaptive dwell policy (`pig_scan_plan()`, flag `PIG_FEAT_ADAPTIVE_DWELL`): 50 ms active fast pass, DFS 5 GHz (ch 52–144) forced passive (compliance). Plus the two-pass **sweep FSM** (`PigSweep`, `pig_sweep_*`) that now drives every node's scan loop: the fast active pass scores each channel by yield, then the passive revisit pass dwells 110–300 ms on the top ~half of the productive channels (max 8). `pig_sweep_build()` + `pig_sweep_valid()`/`_invalidate()` are the firmware-side cycle driver — the sweep is rebuilt whenever the assignment changes. Flag off → fast-only at the legacy fixed active dwell. |
| `lossy_ring.h` | SP/SC lossy ring. Refuses DELTA at ≥80 % fill, overwrites oldest FULL only when saturated — protects the unique-BSSID metric under load. |
| `yield_sched.h` | **Phase 3.** Pure coordinator scoring + yield-packed assignment + `sched_rank` + 3-gate hysteresis, flag `PIG_FEAT_DYNAMIC_ASSIGN`. Orders each dual-band node's 5 GHz channels by measured yield; stays band-aware (2.4-only nodes keep {1,6,11}). |
| `pig_queues.h` | **Phase 4 groundwork.** Cross-core queue types: lossy obs ring, latest-wins GPS mailbox, reject-when-full command ring. |
| `pig_metrics.h` | **Phase 0.** The 1 Hz acquisition metrics line (DESIGN §13) — pure accountant, one `PIGM,...` CSV line per second. Always on. |
| `pig_replay.h` · `tests/replay.cpp` | **Phase 0.** Deterministic A/B scan-corpus replay harness (BASELINE vs DEDUP transmit models), so A/B runs on identical input, not live RF. |

## Feature flags

All default to `0` (legacy behavior) in the headers; the per-board
`platformio.ini build_flags` set them to `1` for the released builds.

| Flag | Phase | What it turns on | Where |
|---|---|---|---|
| `PIG_FEAT_METRICS` | 0 | 1 Hz `PIGM,` metrics line | Core (warhog_fork) — always on |
| `PIG_FEAT_DEDUP` | 1 | worker BSSID dedup | every mesh node |
| `PIG_FEAT_ADAPTIVE_DWELL` | 2 | 50 ms active fast dwell + DFS passive | every mesh node |
| `PIG_FEAT_DYNAMIC_ASSIGN` | 3 | yield-weighted channel assignment (ADMIN_EXT) | Core (warhog_fork, CYD/W550) |
| `PIG_FEAT_CORE_ISO` | 4 | per-record SD logging on a core-0 FreeRTOS task (dual-core S3) behind a cross-core SD mutex | warhog_fork, CYD/W550 |

> **Note:** `PIG_FEAT_CORE_ISO` is dual-core threading. Its ON path was flipped
> on by owner decision; validate SD-queue depth/drops (`[CORE-ISO] SD store q:`)
> under live node load if you hit instability.

## Local testing (no ESP32 toolchain required)

```bash
./scheduler/tests/run.sh
```

Compiles the headers against a tiny `tests/stubs/Arduino.h` shim and runs three
translation units (**175 checks** total):

- `tests/test_all.cpp` (**123**) — every module with the `PIG_FEAT_*` flags at
  their OFF defaults: classification transitions, drop policy,
  scoring/assignment, hysteresis gates, the dwell policy + sweep-FSM ranking and
  revisit selection, CRC check value, and wire sizes.
- `tests/test_dwell_on.cpp` (**33**) — the same sweep FSM compiled with
  `PIG_FEAT_ADAPTIVE_DWELL=1`, which is how the firmwares ship. The flag is a
  compile-time `#if`, so the ON path (passive revisit pass, dwell scaling, DFS
  invariant, and the `pig_sweep_build`/`_invalidate` cycle driver) needs its own
  TU; `test_all.cpp` can only assert the legacy fall-through.
- `tests/test_pair_on.cpp` (**19**) — the Phase 5 WarHog dual-node pair scheduler
  compiled with `PIG_FEAT_PAIR_SCHED=1`: complementary A/B role lists, per-channel
  dwell table, and single-pass (no yield-revisit) sweep.

Logic only — not on-target FreeRTOS timing or radio behavior. Clean under `g++`
and `clang++` at `-O2`.

See [`DESIGN.md`](DESIGN.md) for the full architecture, message schema,
scheduling math, edge cases, benchmarks, and phased rollout.
