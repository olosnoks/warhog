# Testing & Contributing

## Host unit tests (scheduler)
The scheduler headers are pure and host-testable against a tiny `Arduino.h` shim — no
ESP32 toolchain needed. From `scheduler/tests/`:
```bash
./run.sh          # builds + runs: test_all.cpp, test_dwell_on.cpp, test_pair_on.cpp, replay.cpp
```
On Windows (no host g++) run it under WSL:
```bash
wsl bash -c "cd /mnt/c/Users/skons/Desktop/warhog/scheduler/tests && bash run.sh"
```
Current suite: **175 checks, 0 failures** across three TUs plus the replay smoke test.

### The flag / test convention (important)
- `test_all.cpp` compiles the whole suite with **every `PIG_FEAT_* OFF`** and asserts the
  legacy fall-through. So the `#define … 0` defaults in `scheduler/pig_*.h` **must stay 0**.
- A flag's **ON** path is a compile-time `#if`, so it needs its **own translation unit**
  (`test_dwell_on.cpp` for Phase 2, `test_pair_on.cpp` for Phase 5). Add one for any new
  flag's ON behavior and wire it into `run.sh`.

## Firmware build gates
Before pushing a firmware change, build the envs it affects, ON and OFF:
```bash
pio run -d <dir> -e <env>                                  # ON (release defaults)
PLATFORMIO_BUILD_FLAGS="-U<FLAG>" pio run -d <dir> -e <env>  # OFF path still compiles
```
The **OFF path must stay byte-identical** to legacy — new feature code goes behind its
`#if PIG_FEAT_*`.

## CI (`.github/workflows/ci.yml`)
CI runs on **every pull request and push to `main`** (plus manual dispatch) and does not
deploy. It builds all six PlatformIO board envs, runs the scheduler host suite, and runs
the drop-in drift guard (below) — so a broken build or a scheduler regression is caught
before it reaches `main`. A separate `legacy-off-build` job builds the most-flagged env
(freenove) with every `PIG_FEAT_*` scheduler flag undefined, so the OFF/legacy path is
compile-gated too — not just the shipped flags-ON default. Releases and the GitHub Pages
flasher stay with `build-and-deploy.yml` (`v*` tags), where the deploy is gated on the
build so a broken env can't overwrite the live flasher.

## Keeping the C5 header copies in sync
The Arduino sketch `TDongleC5_WarHog/` has no include path, so it keeps **byte-identical
copies** of `pig_proto.h`, `pig_dedup.h`, `bssid_cache.h`, and `pig_dwell.h`. After editing
a scheduler header, re-sync the copy:
```bash
cp scheduler/pig_dwell.h TDongleC5_WarHog/pig_dwell.h   # (and proto/dedup/bssid_cache as needed)
```
These copies are **gated in CI** by `scripts/check-dropins.sh`, which fails the build if any
copy drifts from its `scheduler/` source. Run it locally before pushing:
```bash
bash scripts/check-dropins.sh
```

## Gotchas worth knowing
- **Arduino build artifact:** before committing `warhog_fork`, remove
  `warhog_fork/WarHog.ino.cpp` (a generated artifact; never commit it).
- **S3 flashing** needs esptool `--before usb-reset` (see [Build & Flash](Build-and-Flash.md)).
- A one-off `PLATFORMIO_BUILD_FLAGS` build **overwrites** `firmware.factory.bin` in the
  shared `.pio/build/<env>/` dir — rebuild at release flags before flashing a release image.
- The `build/` directories are Arduino-CLI leftovers and are gitignored — **not** the
  firmware; `.pio/` is the authoritative build output.

## Where state is recorded
- `scheduler/DESIGN.md` — the full technical design (16 sections).
- `scheduler/CLAUDE.md` — current state, flag table, and the "don't relearn these" gotchas.
- `scheduler/PHASE4_PLAN.md` — the core-isolation rollout plan.
- `CHANGELOG.md` — user-facing history.

## License
This repository is **CC BY-NC-SA 4.0** (non-commercial, share-alike). The offensive/dual-use
tools (Marauder, BLE spam, deauth, PMKID) are for authorized testing, research, and CTF use
only — follow your local law and only target systems you're allowed to.
