# Changelog — olosnoks/warhog (fork)

This fork tracks [Hamspiced/piglet](https://github.com/Hamspiced/piglet) upstream and adds
multi-board support, a Marauder/BLE security suite, PigWorld graphics, and a web flasher.
See the upstream [CHANGELOG](https://github.com/Hamspiced/piglet/blob/main/CHANGELOG.md) for
the base firmware history.

## Unreleased

### Batched, PSRAM-backed SD logging + `/archived` folder
- **CSV rows are buffered and written to SD in batches of 100** instead of one
  write per network, with a time-based safety flush (~20 s) so a partial batch
  isn't lost on power-off. The buffer is allocated in **PSRAM** when present
  (freeing ~25 KB of internal RAM) and falls back to internal RAM otherwise.
  Across `warhog_fork`, `WarHog_Freenove_S3` (S3 + W550), TDongleC5, and Waveshare.
- **PSRAM enabled on the XIAO C5 build** (`board_build.arduino.memory_type=qio_qspi`)
  so the batch buffer actually lands in PSRAM there.
- **`/archived` folder.** Files the firmware would auto-delete (empty/header-only
  CSVs from the boot/upload cleanup) are moved to `/archived` instead of being
  erased, so the device never silently removes a file. User-initiated Web UI
  deletes are unchanged. (`warhog_fork`, S3/W550, TDongleC5)

### Wardrift (`wardrift.net`) upload fixes
- **CSV rotation cap lowered 29 → 9 MiB** so every log fits Wardrift's
  `/v1/ingest/signals` raw-upload limit (10 MiB / 200k records); still well within
  WiGLE/WDGWars limits.
- **Correct 409 / duplicate handling.** A real duplicate (`200` + `"duplicate":true`)
  is detected; a `409` is treated as a transient sequence-conflict and the file is
  left in `/logs` for retry instead of being moved out as if accepted.

### Wardrive Go comms parity with upstream Piglet (`IosEspCompat`)
- **Numeric `WD:CAPS fw=1<major>.<minor>.00`** (derived from `FIRMWARE_VERSION`) so
  the Wardrive Go / WarBLE Go app's number-by-number "outdated firmware" check
  accepts the board — was the non-numeric `warhog-1.0`. Applied to `warhog_fork`
  and the main S3/W550 build.
- **Real `bands`/`chans` in `WD:CAPS`** (2g,5g + full channel list on a C5 or Core,
  else 2g + ch 1–14), and the phone's **`-ch` channel plan** is honored in `WD:PLAN`,
  in forwarded APs, and in the Solo scan loop (scans only the requested channels,
  reverting to a full sweep when the phone disconnects).

### Web flasher
- Board **dropdown** (one card at a time) replaces the all-boards grid; a **How to
  use the flasher** card; and a **What's new** card that renders this
  `CHANGELOG.md`'s newest section live (copied into the site at deploy time).

### Fix saved cluster profiles not loading after a reboot (CYD/W550)
- **Profiles saved on the Core didn't come back after a CYD reboot.** Same root cause as the
  settings-persistence fix below: `meshProfilesSaveToSD()` and `meshProfilesLoad()` accessed
  `/mesh_profiles.cfg` without the shared SD+TFT `SpiBusLock`, so in Core mode they could collide
  with the display flush / SD-log task. The save failed silently (the UI still reported success)
  and the load returned nothing. Both now hold the lock; the save also `flush()`es before closing
  and logs `[PROFILE] Saved /mesh_profiles.cfg` or a failure to open the file on serial.
- Verified on a Freenove CYD (assign nodes to sub-clusters → save profile → reboot → load). Not
  yet tested on the Guition JC8048W550.

### Wardrive Go (`iosESP-XXXX`) bridge + solo PigWorld defaults on XIAO C5 (`warhog_fork`)
- **Wardrive Go support across Solo, Mesh Node, and Mesh Core modes.** Added `IosEspCompat`
  (`iosESP-XXXX` GATT service + primary UUID advertising, disconnect/stop-advertising handling so
  iOS does not drop with reason 531) alongside Biscuit Manager (`blePersonality=wardrivego` default,
  configurable in `wardriver.cfg` and the Web UI). Scans and GPS stream over BLE in Solo, Node, and
  Core modes, including a lightweight ch-6 self-scan when in lone-Core mode with no linked nodes.
- **Boot straight into animated PigWorld in Solo wardriving mode.** Default `meshModeOnBoot` is now
  `none` (Solo mode on page 4 with randomized PigWorld terrain/sky and active scanning even while
  the SoftAP window is idle or without an SD card / GPS fix).

### Fix the Wardrive Go phone bridge and settings persistence (CYD/W550)
- **Wardrive Go couldn't see the Core.** The iosESP/GhostESP bridge was advertising as
  `WarHog-XXXX` (a `Piglet-XXXX` branding from before, then caught by the WarHog rename), but
  the RocketGod Wardrive Go / WarBLE Go app scans for the protocol name `iosESP-XXXX`. Restored
  that name so the app discovers the device again. The Biscuit Manager bridge was unaffected
  (it matches on service UUID, not the GAP name).
- **Settings didn't persist across reboots.** On the CYD the SD card and TFT share one SPI bus;
  `saveConfigToSD()` wrote without the `SpiBusLock`, so a settings change in Core mode collided
  with the UI/SD tasks on the bus and the write silently failed. It now takes the shared-bus
  lock (no-op when `PIG_FEAT_CORE_ISO` is off).
- **Settings UX:** the phone-app selector is renamed *CORE BLE APP* → *CORE PHONE APP* and the
  `BISCUIT REPORT` hint clarified, so it's no longer confused with the phone app — `biscuitReport`
  is ESP-NOW forwarding to a separate Biscuit coordinator, `blePersonality` is the phone app.
- **Reboot-flagged choices now auto-apply.** Changing a REBOOT-marked choice (CORE PHONE APP,
  MESH MODE ON BOOT) reboots immediately so the change takes effect cleanly — for the phone app
  that means the device comes back advertising exactly one protocol (Biscuit *or* Wardrive Go),
  no lingering bridge. It only auto-reboots if the save persisted; a failed save shows
  "Last save FAILED" and does not reboot, so the change isn't lost.

### Update mesh nodes over the air from the CYD Core
New ESP-NOW messages `PIG_MSG_OTA_BEGIN/DATA/ACK` (0x25–0x27) let the Core stream a node's
app image (`/fw/<node-env>.ota.bin` on the SD card, staged into PSRAM at Core boot) to a
worker node: select the node on the Core's Nodes screen and tap **UPDATE**. Go-back-N transfer
with a whole-image SHA-256 check before the node switches slot (a bad transfer leaves the old
firmware running). The Core sends each node the image matching its chip (XIAO C5 →
`warhognode_c5.ota.bin`, ESP32-C3 OLED → `esp32c3_oled.ota.bin`). OTA-capable nodes move to a
two-app-slot partition table — **each node needs one USB/web reflash to gain OTA ability**; the
app-only `*.ota.bin` is now also published by CI for every board. A node accepts updates only
from its paired Core MAC (no signature — see the Mesh Protocol doc).

### Phone apps now follow solo wardriving, not just mesh Core mode
The BLE phone bridge used to come up only in mesh **Core** mode, so a device wardriving on its
own was invisible to the app even though every piece of plumbing was already there. All three
Core-capable firmwares now keep the same bridge running while **solo wardriving** and feed it
from the local scanner, behind a new `blePhoneSolo` setting (**default on**).

- **XIAO C5** (`warhog_fork`) — serves the WarHog app's `BLEBridge` GATT plus the attached
  Biscuit Manager service. Records are mirrored from `processScanResults()`; the status payload
  already reported `core:false`, `nodes:0` and the local 2.4/5 GHz found counters, so the app can
  tell a solo unit from a Core with no app change. Toggle lives in the web UI
  (*Phone App Over BLE While Solo Wardriving*) and in `wardriver.cfg`.
- **CYD + W550** — new `phoneBridgeSoloTick()` / `phoneBridgeSoloPushAp()` stand up whichever
  app the existing `blePersonality` setting selects (Biscuit Manager or Wardrive Go), tick its
  commands/telemetry, and push GPS at ~1 Hz. Toggle is on the Settings page as
  *PHONE APP SOLO*. This is distinct from the existing `biscuitReport`, which forwards over
  **ESP-NOW** to a Biscuit *Core* rather than to a phone.
- **Radio ownership:** on the S3 boards the bridge shares one NimBLE stack with the wardriving
  BLE scanner and the Marauder detectors/spam tools. Advertising and GATT coexist with a scan,
  but `NimBLEDevice::deinit()` does not — so `biscuitCompatStopStandalone()` /
  `iosEspCompatStopStandalone()` took a `freeStack` argument (default `true`, mesh Core path
  unchanged) and the solo teardown frees the controller only when `bleScannerActive()`,
  `bleDetectorsActive()`, `appleBleSpamActive()` and `bleSpamActive()` are all false. Entering
  any mesh role tears the solo bridge down first.

Not covered: the T-Dongle sketch has no BLE stack of any kind, and its flasher card serves
Hamspiced upstream's release rather than this tree.

### Yield-driven mesh scheduler — now LIVE (on by default)
A staged rework of mesh acquisition toward yield-driven scheduling. The feature flags are now
**enabled by default in the released flasher builds** (set per board in `platformio.ini`
`build_flags`); each still compiles to the legacy behavior when its `-DPIG_FEAT_*=0`, so any
one phase can be disabled in isolation. Host-tested (156 checks: `./scheduler/tests/run.sh`);
full design in `scheduler/DESIGN.md`, Phase-4 plan in `scheduler/PHASE4_PLAN.md`.

- **Phase 0 — instrumentation** (`PIG_FEAT_METRICS`): a 1 Hz `PIGM,…` metrics line (DESIGN §13)
  on the XIAO C5 Core + a host A/B replay harness. Validated on hardware (212 B/unique baseline).
- **Phase 1 — worker BSSID dedup** (`PIG_FEAT_DEDUP`, all node firmwares): suppresses redundant
  repeat sightings before a node transmits (`scheduler/pig_dedup.h`). **~29% mesh airtime saved**
  (stationary) measured on hardware; surfaces as `supp_pct` on the Core's PIGM line.
- **Phase 2 — two-pass adaptive sweep + DFS-passive** (`PIG_FEAT_ADAPTIVE_DWELL`, all node
  firmwares): every node's channel sweep is now driven by the `PigSweep` FSM
  (`scheduler/pig_dwell.h`). A **50 ms active fast pass** covers the whole assignment and scores
  each channel by what it yielded, then a **passive revisit pass** (110–300 ms, scaled by that
  yield) dwells deeper on the top ~half of the productive channels, up to 8 — depth where the APs
  actually are, without extra probe traffic. DFS 5 GHz (ch 52–144) is forced passive on both
  passes for compliance, and channels the radio cannot scan (5 GHz on 2.4-only silicon) are
  dropped when the sweep is built instead of burning a scan slot. With the flag off the sweep is
  fast-only at the legacy fixed active dwell, i.e. exactly the old behavior.
- **Phase 3 — dynamic yield assignment** (`PIG_FEAT_DYNAMIC_ASSIGN`, Core: warhog_fork + CYD):
  tracks per-channel yield and hands each node an explicit yield-ordered channel list
  (`PIG_MSG_ADMIN_EXT`); stays band-aware so 2.4 GHz-only nodes keep 1/6/11.
- **Phase 4 Step 1 — core-isolated SD logging** (`PIG_FEAT_CORE_ISO`, warhog_fork + CYD/W550):
  the Core's per-record WiGLE SD write moves to a FreeRTOS task (core 0 on the dual-core S3) so
  the ESP-NOW RX drain keeps running; on the CYD the SD card and TFT share one SPI bus, so a
  recursive `gSpiBusMutex` (`SpiBusLock.h`) serializes the display flush against the SD task.
  Instrumented via a 5 s `[CORE-ISO] SD store q: depth/drops` line.

**Caution:** `PIG_FEAT_CORE_ISO` is dual-core threading whose ON path has not been validated on
hardware; it was enabled by owner decision. Watch the `[CORE-ISO]` queue line under live node
load and disable the flag if a board becomes unstable.

### CYD Core-mode screen lag fix
Channel reassignment ran a blocking `delay(10)`-per-node pass on *every* `CORE_REQUEST`, stalling
the loop; now coalesced to at most one reassign per 800 ms and only on an actual roster change.

### CYD mesh: sub-clusters, cluster UI & wardriving profiles
On-device cluster management for the **Freenove S3 CYD (HAM UI4)** firmware, in Core mode:
- **Cluster view** — the Home STATUS card (in Core mode) drills into the live node list.
  Each worker card is tappable and shows its sub-cluster tag and the exact channels it is
  scanning.
- **Sub-clusters** — tap a node to open a per-node editor and move it between sub-clusters
  (`CL1…CL9`). The Core now splits channels *within* each sub-cluster, so co-located nodes
  sharing one antenna never scan the same channel at once, while separate clusters each
  cover the spectrum independently. Splitting stays band-aware (2.4 GHz-only nodes get
  1/6/11; dual-band nodes share the 5 GHz space).
- **Wardriving profiles** — a built-in **DEFAULT** (even band-aware split across all nodes)
  plus **3 named custom slots** saved to the SD card (`/mesh_profiles.cfg`). Save the
  current grouping, then recall a setup with one tap instead of re-grouping each session;
  groupings are remembered by MAC so a node that drops and rejoins returns to its cluster.
- All CSV logs across the fork firmwares now rotate under a hard 30 MB ceiling (byte-based).

### Upstream sync — now at v2.7
Brought the shared firmware up from the v2.62 fork point to upstream **v2.7**, keeping all
fork additions (BLE bridge, PigWorld, herd-count display, PlatformIO builds):

- **WarHog Core** (`warhog_fork`): Wardrift upload (3rd upload service),
  SSID whitelist (`ssidWhitelist1–10`), home-network auto-return + `uploadOnNetworkSeen`,
  per-file WDGoWars upload fix, upload size logging, and the GPIO0 boot-settle guard.
  Biscuit handling keeps the fork's BLE-bridge mirror and herd-count branch **and** adds
  upstream's Biscuit plain-reply (encrypt=0) handshake.
- **WarHogNode C5** and **T-Dongle C5**: synced to upstream v2.7 (clean drop-in).
- `wardriver.cfg` sample gained `wardriftApiKey`, `ssidWhitelist1–10`, `uploadOnNetworkSeen`,
  and `sdMaxSpiHz`.
- *Waveshare C6*: no change (upstream had none since v2.62).

### Web flasher
- Re-added the **LilyGo T-Dongle C5** card (official build — flashes upstream's prebuilt
  `TDongleC5_Piglet.ino.merged.bin`, latest).
- The WarHog Core (XIAO C5) and WarHogNode C5 cards carry a **skon's fork / official**
  toggle that tracks ham's latest GitHub release; the fork side is now synced to v2.7.

### In progress
- Porting the v2.63–v2.7 feature set into the fork-only **CYD/Guition** suite and
  **c3_node** (separate branch; these trees are full rewrites with no upstream lineage).
