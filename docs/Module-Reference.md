# Module Reference

Firmware source modules and their public API. Signatures are from the S3 project
(`WarHog_Freenove_S3_CYD_HAM_UI4/src/`); the C5 Core (`warhog_fork/`) has
the same subsystems plus `PigWorld` and `PigMetricsHook`. Internal `static` helpers are
omitted — follow the `.cpp` for those.

Everything runs from `main.cpp`: each module exposes a `…Begin()` called in `setup()` and
a `…Tick()` called every `loop()`.

---
## Scanning & logging
### `Scanner` — solo Wi-Fi wardriving
- `void doScanOnce()` — kick/collect one async all-channel scan; on completion calls
  `processScanResults()` which writes rows and mirrors to the phone bridge. Dwell is
  100 ms (200 ms in powersave). *Not* used in mesh node/core mode.

### `SDUtils` — WiGLE/CSV file I/O and path safety
- `bool openLogFile()` / `void closeLogFile()` — open/rotate `/logs/WiGLE_*.csv`.
- `bool appendWigleRow(mac, ssid, auth, ts, ch, rssi, lat, lon, alt, acc)` — one Wi-Fi row.
- `void appendWigleBleRow(mac, name, rssi, …)` — one BLE row.
- `String normalizeSdPath(dir, name)`, `String pathBasename(p)`, `bool isAllowedDataPath(p)`,
  `bool moveToUploaded(srcPath)` — path hygiene + post-upload archival. The pure path/name
  helpers (`normalizeSdPath`, `pathBasename`, `isAllowedDataPath`, `sanitiseDeviceName`) live
  once in `common/sd_paths.h` (shared via `-I../common`, like `scheduler/`), so the Core and
  CYD SDUtils no longer keep divergent copies.

### `SessionStats` — counters and history
- `statsRecordWifiAuth(mode)` — tally per auth type for the stats screen.
- `statsHistoryTick()` / `statsHistoryLen()` / `statsHistoryGet(i, wifi, ble)` — rolling history.
- `statsRateTick()` / `float radiosPerMinute()` — live acquisition rate.

### `HashcatWriter` — handshake/PMKID capture → Hashcat 22000
- `hashcatWriterOpen(ssidHint)` / `Close()` / `bool hashcatWriterIsOpen()`.
- `IRAM_ATTR hashcatWriterObserve(frame, len)` — called from the RX path to extract EAPOL.
- `hashcatWriterHandshakeCount()` / `hashcatWriterPmkidCount()` / `hashcatWriterCurrentPath()`.

See [Scanning & Logging](Scanning-and-Logging.md).

---
## Mesh
### `MeshNode` — ESP-NOW coordinator & worker
- Node: `enterNodeMode()`, `exitNodeMode()`, `nodeModeTick()`.
- Core: `enterCoreMode()`, `exitCoreMode()`, `coreModeTick()`.
- Sub-clusters: `coreSetNodeGroup(nodeIdx, group)`, `uint8_t coreGroupMemberCount(group)`.
- Saved cluster layouts: `meshProfilesInit()` (load from SD), `meshApplyDefault()` (even split).
  Profiles persist to `/mesh_profiles.cfg` (`P <slot> <name>` / `N <MAC> <group>` lines). Save and load
  hold the shared SD+TFT `SpiBusLock`; the save logs `[PROFILE] Saved ...` on serial.
- Solo phone bridge (lives here so it can be torn down on mode change):
  `phoneBridgeSoloTick()`, `phoneBridgeSoloPushAp(bssid, ssid, …)`, `bool phoneBridgeSoloActive()`.

Full protocol in [Mesh Protocol](Mesh-Protocol.md); scheduling in [Adaptive Scheduler](Adaptive-Scheduler.md).

---
## BLE wardriving & detectors
### `BleScanner`
- `bleScannerBegin()`, `bleScannerTick()`, `bleScannerShutdown()` (release the BT controller
  before entering the ESP-NOW mesh), `bool bleScannerActive()`, `uint32_t bleUniqueCount()`.

### `BleDetectors` (+ `AirTagMonitor`, `FlipperSniff`, `PwnagotchiDetect`)
- `bool bleDetectorsActive()` — true while any passive BLE detector owns the stack.
- The detectors classify advertisements (Apple FindMy/AirTag, Flipper Zero, Pwnagotchi)
  and log/annotate them. See [BLE & Detectors](BLE-and-Detectors.md).

### `BleSpam` / `AppleBleSpam` — advertisement floods
- `bleSpamBegin/Start(type)/Stop/Tick`, `bool bleSpamActive()`, `bleSpamCount()`,
  `const char* bleSpamTypeName(type)`.
- `appleBleSpamBegin/Start/Stop/Tick`, `appleBleSpamActive()`, `appleBleSpamCount()`.

---
## Security tools (Marauder port)
### `MarauderTools` — orchestrator
- `marauderToolsBegin()`, `marauderToolsTick()`.
- `bool marauderStartAttack(AttackMode mode, uint8_t channel)` / `marauderStopAttack()` /
  `bool marauderAttackActive()` / `AttackMode marauderCurrentAttack()` / `marauderCurrentChannel()`.
- Beacon config: `marauderSetBeaconConfig(src, ssid, hop)`, `marauderBeaconSource()`, `marauderBeaconHop()`.
- Counters: `marauderFramesSent/Failed/PerSec/Captured()`.

### `RawCapture` — promiscuous PCAP
- `rawCaptureBegin/Start(filter, ch)/Stop/Tick`, `bool rawCaptureActive()`, `rawCaptureFrames()`,
  `String rawCaptureBreakdown()` (`B:/P:/D:/E:` counts), `IRAM_ATTR rawCaptureOnFrame(...)`.

### `DeauthDetector`
- `deauthDetectorBegin/Start(ch)/Stop/Tick`, `deauthDetectorCount()`, `deauthDetectorUniqueSources()`,
  `String deauthDetectorStatusLine()`, `bool deauthDetectorAlert()`, `IRAM_ATTR deauthDetectorOnFrame(...)`.

### `PmkidCapture`
- `pmkidBegin()`, `bool pmkidStart(bssid, ssid, ch)`, `pmkidStop/Tick`, `bool pmkidActive()`,
  `pmkidAttempts()`, `pmkidCaptures()`, `String pmkidCurrentPath()`, `pmkidStatusLine()`,
  `pmkidStopReason()`, `IRAM_ATTR pmkidOnFrame(...)`.

### `WpsCheck`
- `int wpsScanRun()`, `wpsApCount()`, `wpsEnabledCount()`,
  `bool wpsGet(i, ssid, bssid, ch, rssi, wps)`, `bool wpsCheckAp(i)`, `const char* wpsLabel(i)`,
  `bool wpsLogResults()`.

See [Marauder Security Tools](Marauder-Security-Tools.md).

---
## Companion-app bridges
### `BiscuitCompat` — Biscuit Manager GATT
- `biscuitCompatAttach(server)` / `Detach()`, `biscuitCompatBeginStandalone(name)` /
  `StopStandalone(freeStack)`, `bool biscuitCompatActive()`, `biscuitCompatTick()`.
- Push: `biscuitCompatPushAp(bssid, ssid, …)`, `biscuitCompatPushGps(lat, lon, alt, acc)`.
- Node roster snapshots: `biscuitCompatConsumeSnapshotRequest()`,
  `biscuitCompatSnapshotBegin/Record/Complete()`.

### `IosEspCompat` — Wardrive Go (iOS) GATT
- `iosEspCompatAttach/Detach`, `BeginStandalone(name)` / `StopStandalone(freeStack)`,
  `bool iosEspCompatActive()`, `Tick()`, `PushAp(...)`, `PushGps(lat, lon, alt, sats)`,
  `iosEspCompatSetGpsPresent(present)`.

See [Companion Apps & Bridges](Companion-Apps-and-Bridges.md).

---
## Config, connectivity, uploads
### `Config` — SD-backed settings (`/wardriver.cfg`)
- `bool loadConfigFromSD()` / `bool saveConfigToSD()`.
- `cfgAssignKV(k, v)` + `parseKeyValueLine(...)` — the `key=value` parser.
- `cfgEnforceMutualExclusion()` — resolve conflicting options.
- Board autodetect: `const PinMap& detectPinsByChip()`, `PinMap pickPinsFromConfig()`, `bool wardriverIsC5()`.

### `WiFiManager` — STA/AP transitions, home-network return.
### `WebUI` — `startWebServer()` brings up the on-device HTTP UI/API.
### `WigleUpload` — batch upload to WiGLE and WiGLE-compatible services
- WiGLE: `wigleTestToken()`, `uploadFileToWigle(path)`, `uploadAllCsvsToWigle(max)`, `deleteEmptyCsvs()`.
- WDGoWars: `wdgwarsTestKey()`, `uploadFileToWdgwars(path)`, `uploadAllCsvsToWdgwars(max)`,
  `wdgwarsServicePendingJobs()`, `wdgwarsDrainPendingJobs(budgetMs)`.
- Wardrift: `wardriftTestKey()`, `uploadFileToWardrift(path)`, `uploadAllCsvsToWardrift(max)`.

See [Data & Uploads](Data-and-Uploads.md).

---
## Display, GPS, power
### `ScreenUI` / `Display` / `ScreenTelemetry` / `Gfx` / `LGFX_W550` / `ViewportSprite` / `Touch`
- `ScreenUI::begin/update(speed)/handleTouch/requestRedraw/showSplash/draw` and the mesh/
  core sub-views; `triggerHomeUpload/CoreMode/RadioIdle()` are exposed for serial testing.
- See [Display & UI](Display-and-UI.md).

### `GPS`
- `String iso8601NowUTC()` — the UTC timestamp stamped on every row.
- `headingFeed(deg)` / `double headingSmoothedDeg()` — smoothed travel heading.

### `BatteryMonitor`
- `batteryMonitorTick()`, `bool batteryMonitorAvailable()`, `float batteryVoltage()`, `int batteryPercent()`.

---
## C5 Core extras (`warhog_fork/`)
- **`PigWorld`** — the 1-bit pixel-art animation engine (pig flocking scales 1→6 with node
  count, 2.4 GHz occupancy histogram). See [Display & UI](Display-and-UI.md).
- **`PigMetricsHook`** — hard-wires the Phase-0 `PIGM,` metrics line (see [Serial & Telemetry](Serial-and-Telemetry.md)).
