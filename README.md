# WarHog — Multi-Board Wardriving & Wireless Research Suite

[![Web Flasher](https://img.shields.io/badge/Web_Flasher-Launch_Installer-2dd4bf?style=flat-square)](web-flasher/index.html)
[![License: CC BY-NC-SA 4.0](https://img.shields.io/badge/License-CC_BY--NC--SA_4.0-blue.svg?style=flat-square)](LICENSE)

An extended, multi-board distribution of **WarHog**, an open-source ESP32 wardriving and wireless research platform. WarHog scans Wi-Fi (2.4 GHz and 5 GHz) and Bluetooth Low Energy (BLE) networks, attaches high-accuracy GPS coordinates, logs data in WiGLE-compliant CSV and raw PCAP formats to MicroSD, coordinates worker nodes across an ESP-NOW mesh, and provides touch and web interfaces for field operations.

This repository unifies official upstream Piglet hardware with dedicated ports for Cheap Yellow Displays (CYD), RGB IPS touch panels, micro OLED worker nodes, USB dongles, and a zero-install browser flasher.

---

## 1. Upstream Comparison: What is Different in This Fork?

This repository builds upon upstream [Hamspiced/piglet](https://github.com/Hamspiced/piglet). The shared firmware (WarHog Core, WarHogNode, T-Dongle C5) is kept **in sync with upstream — currently v2.7** (Wardrift upload, SSID whitelist, home-network auto-return, GPIO0 boot-settle fix). The table below is the fork's additions *on top of* that upstream baseline:

| Feature / Subsystem | Upstream (`Hamspiced/piglet`) | This Fork (`olosnoks/warhog`) |
|---|---|---|
| **Supported Hardware** | Seeed XIAO (S3, C5, C6, C3), T-Dongle C5, Waveshare C6 Mini | **+ Freenove S3 CYD (2.8″ Touch)**<br>**+ Guition JC8048W550 (5.0″ 800×480 RGB IPS)**<br>**+ WarHogNode C3 (0.42″ OLED Micro-Node)** |
| **Offensive & Security Tools** | None (pure wardriving & discovery) | **ESP32 Marauder Port:** Raw 802.11 frame injection, beacon spam, deauth attacks, deauth detection, PMKID capture with Hashcat 22000 export, WPS vulnerability checks, and raw PCAP logging to SD |
| **Bluetooth Low Energy (BLE)** | Not supported | **BLE Wardriving & Sniffers:** AirTag/FindMy tracking, Flipper Zero detection, Pwnagotchi detection, and multi-platform BLE advertisement floods (Apple, SwiftPair, Samsung, Fast Pair) |
| **Mesh Ecosystem Support** | JCMK Wardriver ESP-NOW protocol + Biscuit Pro mesh interop (incl. plain-reply, v2.63) | Same mesh base **+ Biscuit Manager BLE/GATT layer** (`BiscuitCompat.cpp`) and **Core BLE Bridge** (`BLEBridge.cpp`) for forwarding BLE records to companion apps |
| **Adaptive Mesh Scheduler** | Static even channel-slicing across nodes | **Yield-driven acquisition suite** ([`scheduler/`](scheduler/)): worker BSSID dedup (~29 % airtime saved stationary), a two-pass adaptive sweep (fast active, then yield-weighted passive revisits) with DFS-passive compliance, dynamic yield-weighted channel assignment (`PIG_MSG_ADMIN_EXT`), and dual-core SD-logging isolation. Flag-gated (`PIG_FEAT_*`), host-unit-tested (175 checks), and **on by default** in released builds |
| **Over-the-Air Node Updates** | USB flashing only | **OTA over ESP-NOW** (`PIG_MSG_OTA_BEGIN/DATA/ACK`): a CYD/W550 Core streams a node its app image from the SD card with a whole-image SHA-256 check — no USB. Chip-matched (XIAO C5 + ESP32-C3 OLED nodes); the Core picks each node's image automatically. Nodes use a two-app-slot partition (one USB reflash to adopt) |
| **Visuals & Graphics Engine** | Standard OLED text/icons and static pig screen | **PigWorld Graphics Engine:** 1-bit pixel art animation loop, dynamic multi-node flocking visualization (scaling 1 to 6 pigs concurrently), and 2.4 GHz channel occupancy histograms |
| **Web Flasher** | Documentation references to IDEs | **Integrated Web Flasher:** Browser-based flashing via WebSerial (ESP Web Tools) with real-time SVG device screen previews |
| **Build Infrastructure** | Arduino IDE sketches | **Reproducible PlatformIO Environments:** Standardized across all targets with pinned `pioarduino` 55.03.39 (ESP-IDF 5.x / Arduino 3.x) toolchains |

---

## 2. Hardware Matrix

Each board target has a dedicated firmware project and full documentation:

| Hardware Project | SoC & Arch | Display & Touch | Wi-Fi Bands | GPS / SD | Primary Mesh Role | Documentation |
|---|---|---|---|---|---|---|
| **[Freenove S3 CYD](WarHog_Freenove_S3_CYD_HAM_UI4/)** | ESP32-S3 (Xtensa Dual-Core) | 2.8″ 240×320 SPI TFT<br>Capacitive Touch | 2.4 GHz | UART GPS<br>4-bit SDIO | Standalone / Coordinator / Security Suite | [CYD Guide](WarHog_Freenove_S3_CYD_HAM_UI4/README.md) |
| **[Guition JC8048W550](WarHog_Freenove_S3_CYD_HAM_UI4/)** | ESP32-S3 N16R8 | 5.0″ 800×480 RGB IPS<br>Goodix GT911 Touch | 2.4 GHz | UART GPS<br>SPI SD | Standalone / Coordinator / Dashboard | [Guition Guide](WarHog_Freenove_S3_CYD_HAM_UI4/README.md) |
| **[WarHogNode C3](c3_node/)** | ESP32-C3 (RISC-V Single-Core) | 0.42″ 72×40 SSD1306 OLED<br>Physical Button | 2.4 GHz | Headless<br>(No GPS/SD) | Worker Node (Autonomous ESP-NOW) | [C3 Node Guide](c3_node/README.md) |
| **[WarHog Core (XIAO C5)](warhog_fork/)** | ESP32-C5 (RISC-V Dual-Band) | 0.96″ SSD1306 OLED<br>Physical Button | 2.4 GHz +<br>5 GHz Wi-Fi 6 | UART GPS<br>SPI SD | Central Coordinator / Standalone | [Core Guide](warhog_fork/README.md) |
| **[LilyGo T-Dongle C5](TDongleC5_WarHog/)** | ESP32-C5 (RISC-V Dual-Band) | 0.96″ ST7735 Color TFT<br>APA102 RGB LED | 2.4 GHz +<br>5 GHz Wi-Fi 6 | UART Qwiic<br>SPI SD | Portable Dongle / Node / Core | [T-Dongle Guide](TDongleC5_WarHog/README.md) |
| **[Waveshare C6 Mini](waveshareDisplayMiniWarHog/)** | ESP32-C6 (RISC-V Wi-Fi 6) | 1.47″ 172×320 ST7789 TFT<br>Physical Button | 2.4 GHz Wi-Fi 6 | UART Header<br>SPI SD | Standalone Wardriver | [Waveshare Guide](waveshareDisplayMiniWarHog/README.md) |
| **[WarHogNode C5](WarHogNode/)** | ESP32-C5 (RISC-V Dual-Band) | None (LED Status Only) | 2.4 GHz +<br>5 GHz Wi-Fi 6 | Headless<br>(No GPS/SD) | Worker Node (Dual-Band Mesh) | [C5 Node Guide](WarHogNode/README.md) |

---

## 3. Directory Map

```text
warhog/
├── warhog_fork/                # XIAO C5 Core firmware (BLE bridge, BiscuitCompat, PigWorld)
├── c3_node/                    # ESP32-C3 0.42″ OLED WarHogNode with flocking graphics
│   ├── core-patch/             # Patch adding BLE WiGLE logging to WarHog Core
│   └── docs/                   # Sprite pipelines and visual assets
├── WarHog_Freenove_S3_CYD_HAM_UI4/ # CYD 2.8″ & Guition 5″ touch suite (Marauder/PCAP/PMKID/BLE)
├── WarHogNode/                 # Headless dual-band XIAO C5 mesh worker node
├── TDongleC5_WarHog/           # LilyGo T-Dongle C5 USB-A standalone build
├── waveshareDisplayMiniWarHog/ # Waveshare ESP32-C6 1.47″ vertical color TFT build
├── scheduler/                  # Shared adaptive-mesh-scheduler headers + host tests (-I../scheduler)
├── common/                     # Shared firmware headers (-I../common): SD path helpers
├── web-flasher/                # Browser-based WebSerial firmware flasher
│   └── manifests/              # ESP Web Tools JSON manifests
├── Case Files/                 # 3D printable enclosures (STL / STEP)
└── PCB Files/                  # Hardware schematics and gerbers
```

---

## 4. Quick Start: Flashing Your Device

### Option A: Web Flasher (No Software Installation)
1. Open the **[Web Flasher](web-flasher/index.html)** in Google Chrome, Microsoft Edge, or Brave.
2. Connect your ESP32 board over USB.
3. Select your hardware variant, click **Install**, select your serial COM port, and flash directly in seconds.

### Option B: PlatformIO (CLI / VS Code)
Each firmware folder contains a standalone `platformio.ini` pinned to the `pioarduino` 55.03.39 platform:

```bash
# Example: Flashing the Freenove S3 CYD
cd WarHog_Freenove_S3_CYD_HAM_UI4
pio run -e freenove-s3-cyd-ham-ui4 -t upload

# Example: Flashing WarHogNode C3
cd c3_node
pio run -t upload

# Example: Flashing XIAO C5 Core
cd "warhog_fork"
pio run -t upload
```

> **Note on ESP32-C5 / C6 Flashing:** Onboards featuring native USB-Serial/JTAG may drop connection if `esptool` attempts to load its flasher stub. All PlatformIO configurations include `--no-stub` to flash directly through the hardware ROM loader without requiring manual button resets.

---

## 5. Wireless Mesh Architecture

WarHog uses an ESP-NOW mesh protocol operating on channel 6. The Core no longer
hands workers a flat, even slice of the channel list: with the **yield-driven
scheduler** live (see [`scheduler/`](scheduler/)), it tracks per-channel yield
from the records it receives and assigns each node an explicit, yield-ordered
channel list (`PIG_MSG_ADMIN_EXT`), while workers dedup repeat BSSIDs before
transmitting and dwell adaptively (fast active pass, DFS 5 GHz always passive).
Every feature is flag-gated (`PIG_FEAT_*`) and falls back to the legacy even
split when disabled.
```
           ┌──────────────────────────────────────────────┐
           │                 WarHog Core                  │
           │  (XIAO C5, Freenove S3 CYD, or Biscuit Pro)   │
           │         - GPS Timestamping & Fix             │
           │         - WiGLE CSV & PCAP Logging           │
           │         - Channel Assignment Coordinator     │
           └───────▲──────────────────────────────▲───────┘
                   │                              │
     ESP-NOW Ch 6  │ Results                      │ ESP-NOW Ch 6
     Channel Slice │ & Heartbeats                 │ Channel Slice
                   │                              │
    ┌──────────────┴──────────────┐ ┌─────────────┴──────────────┐
    │        WarHogNode C3        │ │        WarHogNode C5        │
    │  - 2.4 GHz Scan Dwell       │ │  - Dual-Band 2.4 + 5 GHz   │
    │  - 0.42" PigWorld Display   │ │  - 40 JCMK Channels        │
    │  - Multi-Node Flocking      │ │  - Headless Ultra-Low Power│
    └─────────────────────────────┘ └────────────────────────────┘
```

---

## 6. Credits & Attribution

* **Piglet Core & Concept:** Created by [Hamspiced](https://github.com/Hamspiced/piglet).
* **ESP32 Marauder & JCMK Protocol:** Developed by [JustCallMeKoko](https://github.com/justcallmekoko/ESP32Marauder).
* **Biscuit Pro Ecosystem:** Designed by [Hedge / Biscuit Shop](https://biscuitshop.us).
* **Graphics, Multi-Board UI & Web Flasher:** Developed and curated by skonSolo / olosnoks.
