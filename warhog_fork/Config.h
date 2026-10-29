#pragma once
#include "PinMapDefs.h"
#include <Arduino.h>

struct Config {
  String wigleBasicToken;
  String homeSsid;
  String homePsk;
  String wardriverSsid = "WarHog-WARDRIVE";
  String wardriverPsk = "wardrive1234";
  uint32_t gpsBaud = 9600;
  String scanMode = "aggressive"; // aggressive | powersaving
  String board = "auto"; // auto | s3 | c5 | c6 | c3 | exp  (pins selected at
                         // boot; reboot required after change)
  String speedUnits = "kmh"; // kmh | mph
  int battPin = -1; // GPIO for battery voltage ADC (-1 = disabled). Expects 1:2
                    // voltage divider from LiPo.
  bool batteryTest =
      false; // Enable battery test (logs elapsed time to /battery_test.csv)

  // Boot auto-upload limit:
  //  -1 = upload ALL files at boot (no limit)
  //   0 = disabled (no auto-upload at boot)
  //  1+ = upload up to N files at boot (WiGLE allows 25 API calls/day)
  // IMPORTANT: Requires PSRAM enabled in Arduino IDE for reliable TLS
  // connections.
  int maxBootUploads = 25;

  // WDGoWars API key from https://wdgwars.pl/profile -> "Generate API key".
  // If set, CSVs are uploaded to WDGoWars BEFORE WiGLE at every boot.
  // Leave empty to disable WDGoWars uploads.
  String wdgwarsApiKey;

  // Wardrift API key (wdk_...) from wardrift.net, sent as "X-API-Key".
  // The CSV file is uploaded as-is (raw body, Content-Type: text/csv,
  // X-File-Name header) to POST /v1/ingest/signals -- no multipart wrapper
  // and no parsing needed. If set, CSVs are uploaded to Wardrift AFTER
  // WDGoWars but BEFORE WiGLE at every boot (2nd in the chain). Leave empty
  // to disable Wardrift uploads.
  String wardriftApiKey;

  // Optional device name — appended to WiGLE CSV header and filename so
  // multiple WarHogs uploading to the same account can be distinguished.
  // E.g. deviceName=rover1  →  device=WarHog-rover1  /
  // rover1_WarHog_WiGLE_....csv Leave empty for default ("WarHog-Wardriver" /
  // "WiGLE_....csv").
  String deviceName;

  // Auto-start mesh mode after boot uploads: none (default), node, or core.
  // none — normal solo wardriving mode (default: boots straight into the
  //        animated random PigWorld scene in solo wardriving mode).
  // node — become a scanning node that forwards records to the Core.
  // core — become the mesh coordinator (receives wardriving records from
  // nodes).
  String meshModeOnBoot = "none";

  // Rotate the OLED display 180° (true = upside-down mount, false = normal).
  // Requires reboot to take effect.
  bool rotateScreen180 = false;

  // When true: after boot uploads complete, disconnect from home WiFi and
  // begin wardriving immediately instead of staying on the STA connection.
  // The web UI is still reachable if you connect to the Wardriver AP later,
  // but the device will not hold the STA link open. Requires reboot.
  bool autoStartAfterUpload = false;

  // When true (default): if the configured home SSID is seen in a scan
  // result while wardriving, the device stops scanning, connects to it, and
  // uploads pending CSVs automatically. Mutually exclusive with
  // autoStartAfterUpload -- enforced in cfgEnforceMutualExclusion() so the
  // two features can never both be active (one wants to leave home Wi-Fi
  // for good after boot, the other wants to return to it mid-drive).
  bool uploadOnNetworkSeen = true;

  // Ceiling for SD-over-SPI clock negotiation (Hz). Boot tries a descending
  // ladder of speeds up to this cap and uses the fastest one that mounts
  // successfully. Lower this if you see SD write errors/corruption on
  // marginal wiring (e.g. long breadboard jumpers); default is a commonly-
  // safe ceiling for SD-over-SPI. Requires reboot.
  uint32_t sdMaxSpiHz = 20000000;

  // When true (default): keep the BLE phone bridge running while SOLO
  // wardriving and in Mesh Node mode, so the Wardrive Go / Biscuit Manager app
  // receives this device's scan results in every mode (Solo, Node, and Core).
  // In mesh Core mode the bridge always runs; turn this off to disable BLE in
  // Solo and Node modes.
  bool blePhoneSolo = true;

  // BLE phone bridge personality:
  //   "wardrivego" (default) — RocketGod iosESP v1.5 bridge for the Wardrive Go
  //   /
  //                            WarBLE Go app (advertises as "iosESP-XXXX").
  //   "biscuit"              — Biscuit Pro + WarHogMesh GATT advertisement for
  //                            the Biscuit Manager app.
  String blePersonality = "wardrivego";

  // Network whitelist: up to 10 SSIDs that are scanned normally (still count
  // toward Found totals) but are never written to the CSV log -- e.g. your
  // own home/work network you don't want captured. Exact, case-sensitive
  // match. Empty slots are ignored. Applies to both local scanning and
  // mesh-forwarded records written by this device.
  String ssidWhitelist[10];
};

const PinMap &detectPinsByChip();
PinMap pickPinsFromConfig();
bool wardriverIsC5();

String trimCopy(String s);
bool parseKeyValueLine(const String &lineIn, String &keyOut, String &valOut);
void cfgAssignKV(const String &k, const String &v);
bool loadConfigFromSD();
bool saveConfigToSD();

// Forces uploadOnNetworkSeen off whenever autoStartAfterUpload is on, so the
// two mutually-exclusive STA-connection behaviors never both run. Call after
// any bulk config change (file load, Web UI save) -- not from cfgAssignKV()
// itself, since key order within a save is not guaranteed.
void cfgEnforceMutualExclusion();
