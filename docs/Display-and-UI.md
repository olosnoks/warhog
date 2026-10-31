# Display & UI

> **Fork note:** the touch page UI (`ScreenUI`), color themes, the W550 sprite renderer,
> PigWorld, and the telemetry screens are WarHog additions; upstream Piglet targets small
> OLED/TFT status displays. See [Upstream Differences](Upstream-Differences.md).

## Display stacks
The firmware supports several panels behind a small abstraction so the app code draws the
same way regardless of hardware.

| Board | Driver | Notes |
|---|---|---|
| Freenove S3 CYD | TFT_eSPI (ILI9341, SPI) | SPI 10–13; the SD card is on its own SDIO bus |
| Guition JC8048W550 | LovyanGFX (800×480 RGB) | renders to a RAM sprite, then scale-blits |
| XIAO C5 / WarHogNode C3 | SSD1306 OLED (I²C) | small monochrome |
| T-Dongle C5 | ST7735 color TFT | + APA102 status LED |
| Waveshare C6 | ST7789 TFT | 172×320 |

Relevant files: `Gfx.h` (draw abstraction), `LGFX_W550.h` (W550 LovyanGFX config),
`ViewportSprite.h` (the W550 offscreen sprite + scale-blit), `Display.cpp`, `Touch.cpp`
(FT6336 cap touch on the CYD, GT911 on the W550 — both I²C, off the SPI bus).

## Screen UI (`ScreenUI.cpp`) — S3 touch frontend
A page-based touch UI. Key entry points:
- `begin()`, `update(speed)`, `draw()`, `requestRedraw()`, `showSplash()`, `handleTouch()`.
- Header/footer/status chips: `drawHeader()`, `drawFooter()`, `drawStatusChip(...)`.
- Mesh mode menu: `MESH_MODES[] = {none, node, core, off}`; selecting one enters that mode.
  The STATUS card on a Core drills into the cluster view (node list + per-node channels).
- Serial-testable hooks: `triggerHomeUpload()`, `triggerCoreMode()`, `triggerRadioIdle()`
  (mirror the `u` / `c` / `o` serial commands — see [Serial & Telemetry](Serial-and-Telemetry.md)).

> `present()` takes the `SpiBusLock` mutex when core isolation is on. That's defensive only:
> neither S3 board puts the TFT and SD card on one SPI bus (see
> [Adaptive Scheduler → Phase 4](Adaptive-Scheduler.md#phase-4--core-isolation)); the lock's
> real job is serializing SD access between the core-0 log task and core-1 SD users.

## Color themes (`cfg.theme`)
The S3 UI palette is 11 runtime tokens (`g_theme` in `ScreenUI.cpp`) — background, card,
card2, input, border, text, muted, accent, warn, danger, blue. A `UiTheme` preset table
holds a handful of community-favorite dark palettes; the active one is chosen by
`cfg.theme` and applied by `themeApplyFromConfig()`:

| `cfg.theme` | Theme |
|---|---|
| `midnight` | Midnight (default — the original teal-on-near-black look) |
| `nord` | Nord |
| `dracula` | Dracula |
| `gruvbox` | Gruvbox |
| `catppuccin` | Catppuccin (Mocha) |
| `tokyonight` | Tokyo Night |

Change it from **SYS → COLOR THEME** on the touch UI (or set `theme=` in `/wardriver.cfg`).
It's **cosmetic only** — no layout or behavior change — and applies live without a reboot
(`persistConfig()` re-applies and redraws). Adding a theme = one row in `THEME_PRESETS`
plus its name in the `THEMES` choice list and `Config` validation. Theming is S3-only
(the OLED/dongle boards have their own minimal UIs).

## Telemetry screens (`ScreenTelemetry.cpp`)
`screenTelemetryObserve(ssid, bssid, rssi, ch)` feeds live acquisition data (recent SSIDs,
RSSI, per-channel activity, logged-row count) that the stats/telemetry pages render. On a
Core this is fed from `coreParseAndLogText` (mesh records); in solo from the scanner.

## PigWorld (`PigWorld.cpp`, C5 Core)
The 1-bit pixel-art animation engine that gives the Core personality:
- a sprite animation loop (`pig_sprites.h`),
- **flocking** that scales 1→6 pigs with the live node count (a visual mesh-size readout),
- a 2.4 GHz **channel-occupancy histogram** driven by `SessionStats`.
It is cosmetic — no effect on scanning — but it's the quickest glance-read of mesh health.

## HAM logo / splash
`HamLogo.h` / `drawPigLogo()` / `drawPigFace()` render the boot splash and branding.
