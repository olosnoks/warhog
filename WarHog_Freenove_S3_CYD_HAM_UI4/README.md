# WarHog S3 CYD & Guition Edition (HAM UI4)

An advanced wardriving, mesh coordinator, and wireless research firmware for ESP32-S3 touch displays. This distribution extends the core WarHog platform into an interactive handheld suite featuring a full touch UI, integrated packet capture, 802.11 injection tools, PMKID harvesting, Bluetooth Low Energy sniffers/spam, and dual hardware target support.

---

## 1. Supported Hardware Targets

The firmware is built and configured via PlatformIO for two distinct hardware platforms:

| Feature | Freenove S3 CYD (`freenove-s3-cyd-ham-ui4`) | Guition JC8048W550 (`guition-jc8048w550`) |
|---|---|---|
| **Board Model** | Freenove FNK0104B / ESP32-S3 CYD | Guition JC8048W550 |
| **SoC** | ESP32-S3, Dual-Core Xtensa LX7 @ 240 MHz | ESP32-S3 N16R8, Dual-Core @ 240 MHz |
| **Memory** | 16 MB Flash, 8 MB OPI PSRAM | 16 MB Flash, 8 MB OPI PSRAM |
| **Display Panel** | 2.8″ TFT (240×320), SPI (`ILI9341_2`) | 5.0″ IPS (800×480), 16-bit RGB Parallel |
| **Display Library** | `bodmer/TFT_eSPI@2.5.43` | `lovyan03/LovyanGFX@^1.1.16` |
| **Touch Controller** | Capacitive / FT6336 (I²C) | Goodix GT911 Capacitive (I²C) |
| **SD Card Mode** | 4-bit SDIO (High Speed) | SPI Mode |
| **GPS Interface** | UART1 (Header: RX=44, TX=43) | UART1 (Header: RX=44, TX=43) |

---

## 2. Hardware Pinout Reference

### Guition JC8048W550
* **Display (RGB Parallel):** Controlled via LovyanGFX in `LGFX_W550.h`. Landscape layout: the touch UI on the left plus a live status panel (cluster roster / recent APs, channel activity, position) on the right — see the [Display & UI wiki](../docs/Display-and-UI.md#w550-landscape-layout)
* **Backlight:** GPIO 2 (PWM dimming supported)
* **Touch (GT911):** SDA = GPIO 19, SCL = GPIO 20, INT = GPIO 18
* **GPS (UART1):** RX = GPIO 44, TX = GPIO 43
* **SD Card (SPI):** CS = GPIO 10, MISO = GPIO 11, SCK = GPIO 12, MOSI = GPIO 13

### Freenove ESP32-S3 CYD (FNK0104B)
* **Display (SPI):** MOSI = GPIO 11, MISO = GPIO 13, SCLK = GPIO 12, CS = GPIO 10, DC = GPIO 46, Backlight = GPIO 45
* **Touch (FT6336):** SDA = GPIO 16, SCL = GPIO 15, RST = GPIO 18, INT = GPIO 17
* **GPS (UART1):** RX = GPIO 44, TX = GPIO 43
* **SD Card (SDIO):** CLK = GPIO 38, CMD = GPIO 40, D0 = GPIO 39, D1 = GPIO 41, D2 = GPIO 48, D3 = GPIO 47

---

## 3. Architecture & Functional Subsystems

### A. Touch User Interface (`ScreenUI`)
The firmware features a custom touch UI (`ScreenUI.cpp`) operating across 6 primary navigation pages:
1. **Home:** Live wardriving dashboard showing total APs, GPS lock status, coordinates, altitude, speed, and real-time scanning mode toggles (**Solo**, **Core**, **Node**, or **Radio Idle**).
2. **Nodes:** Coordinator management screen for JCMK/WarHog ESP-NOW mesh nodes, listing connected node MAC addresses, assigned scan channels, RSSI, and discovery metrics.
3. **Files:** Full SD card file browser with on-device CSV preview, WiGLE column inspection (Network, Security, Position), and one-touch upload to WiGLE/WDGoWars via home Wi-Fi.
4. **System:** Hardware health monitoring (PSRAM, free heap, battery voltage, temperature), log status, and an interactive on-screen touch keyboard for modifying `wardriver.cfg` parameters without removing the SD card.
5. **Marauder Tools:** Dedicated offensive and wireless research toolset menu.
6. **Tools / Speedo:** Full-screen GPS speedometer, heading compass, and high-visibility collection HUD.

### B. Wireless Research & Offensive Security Toolset
The firmware includes a complete port of the ESP32 Marauder toolset and auxiliary wireless assessment tools:
* **Raw 802.11 Frame Injection:** Enabled by linking with `-Wl,--allow-multiple-definition` to override the ESP-IDF `ieee80211_raw_frame_sanity_check()` symbol in `libnet80211.a`.
* **Beacon Spam (`MarauderTools.cpp`):** Broadcasts SSID beacon frames using predefined lists (e.g., Rickroll, random funny names) or nearby sniffed SSIDs, with channel-hopping support.
* **Deauthentication Attacks:** Targeted or broadcast 802.11 deauth/disassociation frames against selected BSSIDs.
* **Deauth Detection (`DeauthDetector.cpp`):** Passive monitoring mode listening for deauth bursts and spoofed management frames in the immediate airspace.
* **PMKID Capture (`PmkidCapture.cpp`):** Clientless WPA/WPA2 PMKID four-way handshake sniffer.
* **Hashcat Export (`HashcatWriter.cpp`):** Automatically formats and writes captured PMKIDs to SD in standard Hashcat 22000 mode syntax.
* **Raw PCAP Capture (`RawCapture.cpp`):** Captures raw 802.11 management, data, or control frames directly to standard `.pcap` files on the SD card for Wireshark analysis.
* **WPS Detection (`WpsCheck.cpp`):** Passive sniffer inspecting beacon Information Elements (IEs) for vulnerable Wi-Fi Protected Setup implementations.
* **Wi-Fi Spectrum Analyzer:** Real-time 2.4 GHz channel occupancy and density histogram.

### C. Bluetooth Low Energy (BLE) Engine
Powered by `NimBLE-Arduino`:
* **BLE Wardriving:** Sniffs and logs BLE advertisements to WiGLE-compliant CSV files.
* **Device Sniffers (`BleDetectors.cpp`):** Real-time alerts and trackers for:
  * Apple AirTags and FindMy accessories.
  * Flipper Zero devices (`FlipperSniff.h`).
  * Pwnagotchi / AI virtual pet devices (`PwnagotchiDetect.h`).
* **BLE Advertisement Spam (`BleSpam.cpp`, `AppleBleSpam.cpp`):** Transmits pairing prompt floods for Apple iOS popups (AirPods, AppleTV), Microsoft SwiftPair, Samsung Galaxy devices, and Google Fast Pair.

### D. Multi-Node Mesh Integration
* **JCMK Coordinator Mode:** Operates as a central hub on ESP-NOW channel 6, distributing channel slices to headless C5, C3, and Waveshare worker nodes.
* **Biscuit Pro Compatibility (`BiscuitCompat.cpp`):** Full compatibility layer for Hedge's Biscuit Shop coordinator protocol.
* **BLE Core Bridge:** Ingests BLE detection records sent from worker nodes and merges them into the active WiGLE CSV stream.

---

## 4. Building and Flashing

Build environments are configured in [`platformio.ini`](platformio.ini) using the `pioarduino` ESP-IDF 5.x / Arduino 3.x platform.

### Freenove S3 CYD
```bash
# Build
pio run -e freenove-s3-cyd-ham-ui4

# Flash (specify your serial port)
pio run -e freenove-s3-cyd-ham-ui4 -t upload --upload-port COMx

# Monitor
pio device monitor -b 115200
```

### Guition JC8048W550
```bash
# Build
pio run -e guition-jc8048w550

# Flash
pio run -e guition-jc8048w550 -t upload --upload-port COMx

# Monitor
pio device monitor -b 115200
```

---

## 5. Configuration (`wardriver.cfg`)

Settings are stored in `/wardriver.cfg` on the root of the SD card and can be modified via the onboard System editor, the Web UI, or directly on PC:

| Setting | Default | Description |
|---|---|---|
| `wifiSSID` | - | Home Wi-Fi network SSID for automatic upload |
| `wifiPassword` | - | Home Wi-Fi password |
| `wigleAPIKey` | - | WiGLE API token (`AID:TOKEN`) |
| `autoUpload` | `false` | Automatically connect and upload when home SSID is detected |
| `meshModeOnBoot` | `none` | Boot directly into `core`, `node`, or `none` |
| `screenTimeout` | `60` | LCD backlight sleep timer in seconds (0 = always on) |
| `touchCalibrated` | `true` | Touch matrix calibration flag |

---

## 6. License & Credits

* Based on the **Piglet** wardriving project by [Hamspiced](https://github.com/Hamspiced/piglet).
* Cyber / Marauder additions adapted from [ESP32 Marauder](https://github.com/justcallmekoko/ESP32Marauder) by JustCallMeKoko.
* Graphics engine, touch UI, and multi-board abstraction by skonSolo / olosnoks.
* Distributed under the terms of the project's open source license.
