# Upstream Differences — WarHog vs Piglet

WarHog is a fork of **[Hamspiced/piglet](https://github.com/Hamspiced/piglet)**. This page
is the developer-level answer to *"what did the fork actually change?"* — subsystem by
subsystem, at the code level. For the product-level summary table see the repository
[`README.md` §1](https://github.com/olosnoks/warhog/blob/main/README.md); for the
name change and why `PIG_*` identifiers persist, see [Naming & Lineage](Naming-and-Lineage.md).

> **Fork relationship.** The GitHub repo was renamed `olosnoks/piglet` → `olosnoks/warhog`
> but is still a GitHub fork of `Hamspiced/piglet` (parent unchanged). The upstream remote,
> and the ability to pull from or PR back to Hamspiced, are intact.

---

## 1. What is shared and kept in sync

The **mesh Core / node / T-Dongle** firmware descends directly from upstream and is kept
**in sync with upstream v2.7** (Wardrift upload, SSID whitelist, home-network auto-return,
GPIO0 boot-settle fix). On top of that baseline the fork keeps upstream's:

- **JCMK ESP-NOW protocol** (the Wardriver/Biscuit lineage) — message types `1..~10`,
  channel-6 coordination, the `BSSID,SSID,AUTH,CH,RSSI,W` text record.
- **Biscuit Pro interop** — a WarHog node can attach to a stock Biscuit Core, and a WarHog
  Core accepts stock nodes. The **212-byte** record size is upstream's constraint, preserved.
- **WiGLE CSV format** and the `/logs/WiGLE_*.csv` layout.
- **Config model** — the `/wardriver.cfg` `key=value` file and most of its keys.

Because of this, the fork's additions are designed to be **backward-compatible on the wire**:
see §5. The parts that are genuinely new (BLE, Marauder, the extra boards, the scheduler, the
flasher) are *additive* and do not exist upstream at all.

---

## 2. Differences by subsystem

| Subsystem | Upstream Piglet | WarHog fork | Where |
|---|---|---|---|
| **Boards** | XIAO (S3/C5/C6/C3), T-Dongle C5, Waveshare C6 | **+ Freenove S3 CYD**, **+ Guition JC8048W550 (800×480 RGB)**, **+ C3 0.42″ OLED micro-node** | [Hardware & Firmware Projects](Hardware-and-Firmware-Projects.md) |
| **BLE** | none | full BLE wardriving, passive detectors, GATT bridges, advertisement floods | [BLE & Detectors](BLE-and-Detectors.md) |
| **Security tools** | none (pure discovery) | Marauder port: capture/PCAP, beacon/deauth, deauth-detect, PMKID→Hashcat, WPS | [Marauder Security Tools](Marauder-Security-Tools.md) |
| **Mesh scheduling** | static even channel-slicing | yield-driven adaptive suite (dedup, two-pass sweep, dynamic assign, core-iso, pair sched) | [Adaptive Scheduler](Adaptive-Scheduler.md) |
| **Node firmware updates** | USB only | OTA over ESP-NOW (`PIG_MSG_OTA_BEGIN/DATA/ACK`): Core streams a chip-matched app image from SD with SHA-256 verify; C5 + C3 nodes | [Build & Flash](Build-and-Flash.md#updating-nodes-over-the-air-core--worker-node) |
| **Companion apps** | ESP-NOW report to a Biscuit Core | **+ BLE/GATT bridges** to phone apps (Biscuit Manager, Wardrive Go) + a Core BLE record bridge | [Companion Apps & Bridges](Companion-Apps-and-Bridges.md) |
| **Uploads** | WiGLE | WiGLE **+ WDGoWars + Wardrift**, chunked/non-blocking | [Data & Uploads](Data-and-Uploads.md) |
| **Display/UI** | OLED text + static pig | touch page UI, color themes, PigWorld flocking, W550 sprite renderer, telemetry screens | [Display & UI](Display-and-UI.md) |
| **Build** | Arduino IDE sketches | PlatformIO envs (pinned `pioarduino` 55.03.39) + CI matrix | [Build & Flash](Build-and-Flash.md) |
| **Distribution** | IDE / manual | browser web flasher (ESP Web Tools) with device-screen previews | [Web Flasher](Web-Flasher.md) |
| **Tests** | — | host-compiled scheduler unit tests (175 checks) + replay harness | [Testing & Contributing](Testing-and-Contributing.md) |

The rest of this page details the non-obvious ones.

---

## 3. New capabilities (no upstream counterpart)

### BLE layer
Upstream is Wi-Fi-only. The fork adds an entire BLE stack: `BleScanner` (BLE wardriving,
geo-deduped, logged via `appendWigleBleRow`), passive detectors (`AirTagMonitor`,
`FlipperSniff`, `PwnagotchiDetect`), and advertisement floods (`BleSpam`, `AppleBleSpam`).
All of it shares **one NimBLE controller**, which forces the radio-ownership discipline
documented in [BLE & Detectors](BLE-and-Detectors.md) — a concern upstream never has.

BLE sightings flow into the **same WiGLE log** as Wi-Fi, distinguished by the record's
type column (`WIFI` / `BLE`). On the mesh this rides the existing text record: the type is
the last field (`…,RSSI,B`), which is exactly what the `c3_node/core-patch` teaches an
upstream Core to parse (see §4).

### Marauder security suite
A port of the ESP32 Marauder toolset, orchestrated by `MarauderTools` as a single
`AttackMode` state machine that owns the radio while a tool runs. This is a **dual-use**
addition absent upstream; the wiki documents it at the module/API level for authorized
testing only (see the note on [Marauder Security Tools](Marauder-Security-Tools.md)). It
also adds the defensive `DeauthDetector` and the `HashcatWriter` (22000-format export).

### Yield-driven mesh scheduler
Upstream slices channels evenly and statically across nodes. The fork replaces that with
the staged, flag-gated scheduler in [`scheduler/`](https://github.com/olosnoks/warhog/tree/main/scheduler):
worker dedup, a two-pass adaptive sweep, dynamic yield-weighted assignment, dual-core
isolation, and the dual-node antenna-pair scheduler. Each phase is a compile-time
`PIG_FEAT_*` flag that falls back to **byte-identical legacy behavior when `0`**, so the
fork can always be reduced to upstream's scheduling for comparison. Full detail in
[Adaptive Scheduler](Adaptive-Scheduler.md).

### Extra display boards and the touch UI
The two S3 display boards (`WarHog_Freenove_S3_CYD_HAM_UI4/`, built as both the Freenove
CYD and the Guition W550) and their `ScreenUI` page-based touch frontend are fork-only.
They add runtime mode switching, cluster views, color themes, and the W550's offscreen
landscape canvas (page column + live side panel, blitted ×1.5 to 800×480). Upstream targets small OLED/TFT status displays only.

### Web flasher + PlatformIO CI
The browser flasher (`web-flasher/`) and the GitHub Actions matrix that builds every board
and deploys it to Pages are fork infrastructure. Upstream ships Arduino sketches. Note the
flasher deliberately keeps **upstream-pointing cards** (`web-flasher/ham/` and the
`*-ham.json` manifests) that flash Hamspiced's own release binaries — those are *not*
renamed and must keep pointing at `Hamspiced/piglet` release assets.

---

## 4. Modified upstream behavior

These are places where the fork **changes** upstream code rather than adding alongside it.

### Core WiGLE log path — BLE record type
`coreParseAndLogText()` was extended so a mesh text record's trailing field selects the
WiGLE `Type` (`W` → `WIFI`, `B` → `BLE`) instead of always logging Wi-Fi. This is the
single change carried as a standalone patch for the **upstream** Core in
[`c3_node/core-patch/warhog-core-ble.patch`](https://github.com/olosnoks/warhog/blob/main/c3_node/core-patch/warhog-core-ble.patch),
so a stock Piglet Core can log BLE records produced by WarHog nodes.

### Phone bridge now runs in solo mode
Upstream's companion-app plumbing only came up in mesh **Core** mode. The fork keeps the
same BLE bridge running while **solo wardriving** (behind `blePhoneSolo`, default on),
feeding it from the local scanner via `phoneBridgeSoloTick()` / `phoneBridgeSoloPushAp()`.
One app now works in both roles. This also introduced the `freeStack` argument on the
bridge teardown calls so the NimBLE controller is only released when no other BLE consumer
(scanner, detectors, spam) is active.

### Scan loop driven by the sweep FSM
In mesh **node** mode the per-channel scan is no longer a fixed even slice; `nodeDoScanTick`
runs the `PigSweep` FSM (or the Phase-5 per-channel table). With all `PIG_FEAT_*` off this
reduces to the legacy fast-only fixed-dwell sweep. **Solo** scanning is unchanged from
upstream's all-channel sweep — the scheduler is a mesh-path concern only.

### Core RX path hardened for many nodes
On dual-core S3 Cores, `PIG_FEAT_CORE_ISO` moves SD logging and the ESP-NOW record drain
onto pinned core-0 tasks with an SPI-bus mutex, so a slow UI/web frame can't stall ESP-NOW
servicing. Upstream runs everything on the single cooperative loop. See
[Adaptive Scheduler → Phase 4](Adaptive-Scheduler.md#phase-4--core-isolation).

---

## 5. Wire & on-disk compatibility

The fork is designed so a mixed fleet degrades gracefully rather than breaking:

| Interface | Compatibility rule |
|---|---|
| **ESP-NOW record** | stays **212 bytes** — Biscuit Pro drops anything smaller. New binary batch types keep the size. |
| **New mesh message types** | all **≥ 0x20** (`PIG_MSG_OBS_BATCH` 0x20 … `PIG_MSG_ADMIN_EXT` 0x23), so a legacy Core ignores them and the node falls back to the contiguous-range `ADMIN`. |
| **Scheduler flags** | each compiles to byte-identical legacy behavior at `0`; host tests assert the OFF default. |
| **WiGLE CSV** | unchanged header/columns; BLE rows reuse the format with the `Type` column. |
| **Config file** | a superset of upstream's keys; unknown keys are ignored, so a shared card round-trips. |

### What is **not** wire-compatible after the rename
The one upstream-visible break is the **mesh identity**, changed deliberately in the
WarHog rename (see [Naming & Lineage](Naming-and-Lineage.md)):

- The ESP-NOW mesh marker is now `WARHOG1` (was `PIGLET1`). **A WarHog node will not see a
  Piglet node** and vice-versa — the whole mesh must run the same marker.
- The BLE advertised name is `WarHog-Core` (was `Piglet-Core`); apps that match the old
  name need updating.

If you need a WarHog device to interoperate with an unmodified upstream mesh, revert the
marker/name strings (they are localized to `MeshNode.cpp` and the BLE bridge) — the record
format itself is unchanged.

---

## 6. What was intentionally *not* renamed

When the project was renamed, these upstream-owned references were left as "Piglet" on
purpose, because they name Hamspiced's project rather than this fork's firmware:

- All `github.com/Hamspiced/piglet` URLs and the upstream release assets
  (`Piglet.ino.merged.bin`, `PigletNode.ino.merged.bin`, `TDongleC5_Piglet.ino.merged.bin`).
- The `web-flasher/ham/` tree and the `*-ham.json` manifests (they flash upstream builds),
  and the "(official)" flasher card labels.
- Attribution lines crediting Hamspiced for Piglet's core and concept.
- The legacy `PIG_*` / `PIGM` code identifiers and the `pig_*.h` scheduler headers — see
  [Naming & Lineage](Naming-and-Lineage.md) for why these stayed.

---

## 7. Tracing a difference back to the code

- A **mesh** behavior → `MeshNode.cpp` + `scheduler/pig_*.h` → [Mesh Protocol](Mesh-Protocol.md) / [Adaptive Scheduler](Adaptive-Scheduler.md).
- A **BLE** behavior → `BleScanner` / `BleDetectors` / `*Spam` → [BLE & Detectors](BLE-and-Detectors.md).
- A **security tool** → `MarauderTools` + the per-tool module → [Marauder Security Tools](Marauder-Security-Tools.md).
- A **UI/board** behavior → `ScreenUI` / `Gfx` / `LGFX_W550` → [Display & UI](Display-and-UI.md).
- A flag-gated change → find its `PIG_FEAT_*` and read the matching scheduler phase.
