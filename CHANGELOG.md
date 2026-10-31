# Changelog — olosnoks/warhog (fork)

This fork tracks [Hamspiced/piglet](https://github.com/Hamspiced/piglet) upstream and adds
multi-board support, a Marauder/BLE security suite, PigWorld graphics, and a web flasher.
See the upstream [CHANGELOG](https://github.com/Hamspiced/piglet/blob/main/CHANGELOG.md) for
the base firmware history.

## Unreleased

### Repo line-ending policy (LF)
- **Added a `.gitattributes` LF policy for the whole tree.** Local working
  copies carried a mix of CRLF/LF (which made cross-project diffs show as 100%
  changed and triggered "LF will be replaced by CRLF" warnings on every commit).
  The committed blobs were already LF — the CRLF was only a Windows checkout
  artifact — so `git add --renormalize` was nearly a no-op: it touched only one
  file actually stored with CRLF (`TDongleC5_WarHog/wardriver.cfg`, now LF,
  content unchanged). The policy marks all text as LF and binary assets
  (images, STL, `.bin`, zip) as binary, so checkouts are consistent and no
  future CRLF blob can slip in. No source change; nothing recompiled.

### Share the SD path helpers via `common/` (end a divergence)
- **Extracted the byte-identical SD path/name helpers into `common/sd_paths.h`.**
  `pathBasename`, `normalizeSdPath`, `isAllowedDataPath` and `sanitiseDeviceName`
  were duplicated verbatim in the Core (`warhog_fork`) and CYD SDUtils; a fix in
  one had to be hand-ported. They now live once in `common/sd_paths.h` (pure
  `String` logic, no board deps), consumed via a new `-I../common` include path
  on both projects, like `scheduler/`. `SDUtils.h` includes it, so every caller
  is unaffected. The genuinely board-specific SD cores (SPI vs SD_MMC recovery,
  `SdReader`, the logging path) correctly stay per-project. No behavior change;
  all three affected envs (`xiao_c5`, `freenove`, `guition`) build green.

### CI: gate the legacy (flags-OFF) firmware path
- **Added a `legacy-off-build` CI job.** Released builds ship every `PIG_FEAT_*`
  scheduler flag ON, and the invariant is that the OFF path stays byte-identical
  legacy. The host suite asserted the OFF *logic* but nothing compiled the
  firmware OFF path, so a broken `#else`/`#ifndef` branch could slip through. CI
  now also builds the most-flagged env (freenove) with every scheduler flag
  undefined, so the legacy branches are compile-gated on every PR/push.

### Phase 4 Step 1: guard upload file-moves on the shared SPI bus
- **Closed a shared-bus gap in the core-isolation lock discipline.** Under
  `PIG_FEAT_CORE_ISO` the per-record SD write runs on a core-0 task (`tStore`)
  that already holds the `SpiBusLock.h` recursive mutex, but `moveToFolder()`
  — the upload-time file move run on core 1 (`WigleUpload`/`WebUI`) — took no
  lock, so it could collide with `tStore` on a board where SD and TFT share the
  SPI bus. Added a `SpiBusGuard` RAII scope (auto-unlocks on every return) around
  the whole move. `PIG_FEAT_CORE_ISO` ships **ON** on both CYD envs, so the guard
  is live in the released CYD firmware: on the **guition** (SPI SD) `moveToFolder`
  and `tStore` are both SD writers, so it prevents a real two-writer bus collision;
  on the **freenove** (SDIO SD) it is harmless serialization. It compiles to a
  no-op only where the flag is unset (host tests). It changes no logged data.

### CYD: recover a wedged SD bus (both backends)
- **SD bus recovery on the Guition W550 and Freenove CYD.** After sustained
  heavy SD I/O the bus can wedge, so file opens fail even though the card is
  intact. Both boards now mount via `sdBeginBestClock()` and clear a wedge with
  `sdTryRecover()` (unmount + remount one rung slower, rate-limited to once per
  4 s, until reboot), invoked on `openLogFile()` open-failure and on a
  `moveToFolder()` "source missing" — the same mechanism the XIAO C5 Core uses.
  - **Guition W550** (SPI SD): clock ladder 20→16→12→8→4→1 MHz, never below 4 MHz.
  - **Freenove CYD** (4-bit SD_MMC): SDIO ladder via `SD_MMC.begin()`
    `maxClkFreqKHz` 20000→16000→8000→4000→1000 kHz; recovery is `SD_MMC.end()`
    then re-`setPins`/`begin()` slower.
  - **Waveshare C6** (shared SPI SD): its `initSD_SharedSPI()` already remounts
    across a clock ladder, so `sdTryRecover()` rate-limits a re-invocation and is
    wired into `openLogFile()`. The XIAO C5 Core and T-Dongle C5 already had this,
    so every SD-logging board now self-heals a wedged bus.

### Pre-merge CI gate + drop-in header drift guard
- **CI now runs before merge, not just on deploy.** A new workflow
  (`.github/workflows/ci.yml`) runs on every pull request and push to `main`:
  it builds all six PlatformIO board envs and runs the scheduler host test
  suite, so a broken build or a scheduler-logic regression is caught before it
  reaches `main`. It does not deploy — `build-and-deploy.yml` still owns
  releases (`v*` tags) and the GitHub Pages flasher.
- **Drop-in header drift guard.** `scripts/check-dropins.sh` (run in CI) asserts
  that the T-Dongle Arduino sketch's local copies of `pig_proto.h`,
  `pig_dedup.h`, `bssid_cache.h` and `pig_dwell.h` stay byte-identical to the
  canonical `scheduler/` sources. The Arduino IDE has no include path, so those
  copies exist by necessity; the guard replaces the old "review by hand" process
  so a stale protocol can't ship to one board unnoticed.
- `.gitattributes` pins `*.sh` to LF so a Windows edit can't CRLF-break the CI
  scripts' shebang line.

### SD reads fixed + ~94 KB internal RAM freed (S3 CYD / W550)
- **Fixed garbled SD reads.** Arduino `File::read()` goes through a 4 KB stdio
  buffer that ended up in PSRAM when internal RAM was low. The SD driver then
  DMA'd whole sectors into PSRAM and the CPU read stale cached bytes, so files
  came back mixed with old data. The Toolkit showed this as a "corrupt"
  `wardriver.cfg` that changed on every read. The data on the card was fine; only
  reads were affected. A new `SdReader` (`SDUtils.h`) reads through one aligned
  internal DMA buffer. It is now used for the Toolkit GET, config load, mesh
  profiles and OTA images, CSV uploads (WiGLE, WDGWars, Wardrift), the empty-log
  check, the `/archived` copy fallback, web UI downloads and the zip export,
  and the CSV record viewer.
- **Freed ~94 KB of internal RAM.** The mesh/Core and BLE bridge rings
  (`coreTextBuf`, `biscuitFwd`, `ieTxRing`, `txRing`, `coreMirrorBuf`,
  `cmdRing`, `ieCmdRing`, `gPairSeen`) now live in PSRAM via `psramCalloc()`
  (`PsramAlloc.h`). Static internal RAM dropped from 178 KB to 84 KB, and free
  internal heap at runtime went from ~7.6 KB to ~101 KB.
- `WT HELLO` now also reports `heap`, `big`, `dma` and `psram`: free internal
  RAM, the largest internal block, the largest DMA-capable block and free PSRAM.
- **Other boards:** the Toolkit GET on the XIAO C5, T-Dongle C5 and Waveshare C6
  now uses the same POSIX read into an internal DMA buffer.
- **XIAO C5: Toolkit recovers a wedged SD bus.** After a long run the C5's
  SD/SPI bus can wedge, so the card lists as empty and every file open fails.
  The Toolkit showed an empty card and "no wardriver.cfg". Session start, LS and
  GET now detect this and remount with `sdTryRecover()` before retrying, the
  same way the uploader already did.

### Piglet Toolkit (USB) + firmware attached to GitHub Releases
- **Piglet Toolkit on the web flasher.** Connect over the same USB cable used for
  flashing to browse the SD card (list/download/upload/delete across `/logs`,
  `/uploaded`, `/archived`, `/`) and edit `wardriver.cfg` (structured form + raw
  tab). Backed by a new line-based `WT` serial protocol (base64 + CRC32) in the
  S3/W550, XIAO C5, Waveshare C6 and T-Dongle C5 firmware; a session pauses scanning.
- **Firmware on GitHub Releases.** Pushing a `v*` tag now publishes a Release with
  every board's merged first-flash `.bin` and `.ota.bin` attached — the same images
  the flasher serves.
- Fixed a CYD/W550 build break: `SerialTool.cpp` included `<SD.h>` on top of
  `StorageCompat.h`'s `SD`→`SD_MMC` remap.
- **Toolkit file browser.** The SD explorer now looks and works like a small desktop
  file browser: a window with a title bar showing the current folder, icon toolbar,
  file-type icons (CSV, CFG, PCAP, BIN...), Finder-style selection and a status bar;
  an address bar with Up/Refresh, double-click into folders (starts at the
  card root), sortable Name/Size/Type columns with folders first, click / Ctrl /
  Shift / Ctrl+A selection, toolbar Download / Delete on the selection, upload into
  the current folder (button or drag-and-drop, with a replace prompt), text preview
  for `.cfg`/`.csv`/`.txt`/`.log`/`.json` on double-click, and keyboard shortcuts
  (Enter, Del, Backspace, arrows).
- **CSV log viewer.** Double-clicking a WiGLE `.csv` in the Toolkit opens a
  Networks view: summary cards (networks, Wi-Fi / BLE, unique SSIDs, open Wi-Fi,
  GPS-fix share, dates, strongest signal), then a searchable, sortable table
  with security badges, signal bars, and locations that link to OpenStreetMap. An
  All / Wi-Fi / BLE filter and a Text toggle are included. Unreadable lines are
  skipped and counted. CSVs up to 10 MB can be previewed.
- **Text file viewer.** `wardriver.cfg`, `.txt`, `.log` and other text files open
  like a text editor: line numbers, wrapped long lines, dimmed `#` comments,
  highlighted `[sections]` and `key=value` pairs. Non-text bytes are shown as
  faint dots instead of replacement glyphs.
- **Download whole folders, no zip.** Select folders and/or files and click
  Download. Folders are written as plain files, with their structure, into a
  folder you pick, using Chrome/Edge's folder picker. Files alone download
  normally. Without the picker, a folder's files download one by one. There is a
  progress readout and a confirmation above 25 MB.
- **Delete whole folders.** Delete now works on folders. The Toolkit lists
  everything inside, confirms with the folder and file counts and total size,
  then deletes the files and the folders deepest-first. Firmware `WT RM` now also
  removes an empty folder, and refuses the log currently being written
  (`WT ERR busy`), so that log and its folder are kept. Deleting folders needs
  this firmware.
- **Downloads no longer fail on big or flaky files.**
  - The Toolkit's request timeout is now an idle timeout, so a large log no longer
    fails at the old 120-second total limit.
  - A file that arrives short or garbled is rejected instead of saved truncated,
    and is retried once automatically.
  - Firmware GET on every board reports `WT ERR read` on a mid-file read error,
    instead of a `WT END` that made a truncated file look complete.
  - XIAO C5: GET remounts the card and resumes at the same offset after a read
    error. Each SD recovery also lowers the SD clock one step (20 → 16 → 12 MHz,
    not below 4), so a marginal card settles at a speed it can sustain.
- The flasher loads `toolkit.js?v=<build>`, so browsers pick up a new Toolkit
  right after a deploy instead of running a cached copy.
- **Config editor: form only.** Removed the Raw tab; the form still edits the file
  text in place so comments and unknown keys survive. Save is now refused when the
  card's `wardriver.cfg` isn't readable text, so binary data can never be written
  back over a config; a missing file is created on Save.
- **Toolkit connect fixes.** Connecting no longer freezes the page: the serial read
  loop re-acquired a reader in a tight microtask loop when reads failed (e.g. while
  the board rebooted), starving the tab. It now exits cleanly, caps restarts, and
  tears down with timeouts. After opening the port it drives RTS low before DTR
  (RTS-high/DTR-low resets ESP32 USB-Serial/JTAG boards), and the handshake retries
  `WT HELLO` for 15 s so a board that reset on open can finish booting. Boards on
  firmware without the toolkit protocol now get a clear "flash the latest firmware"
  message instead of a hang.

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
