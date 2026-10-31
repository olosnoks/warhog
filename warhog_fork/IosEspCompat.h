#pragma once
#include <Arduino.h>
#include <NimBLEDevice.h>

// ================================================================
//  Wardrive Go / WarBLE Go  (RocketGod "iosESP") BLE bridge personality
// ================================================================
// Makes the WarHog look like a RocketGod "iosESP" wardrive bridge so the
// iPhone Wardrive Go / WarBLE Go app pairs with it and plots the APs the
// WarHog (and its mesh) hears, live on the map.
//
// Protocol: iosESP v1.5 (PROTOCOL.md in the wardrive-bridge package).
//   GATT service  47686F73-7445-5350-4272-696467655376  (advertised)
//     RX   0147686F-7374-4553-5042-726964675852  write / write-NR  (phone -> board cmds)
//     TX   0247686F-7374-4553-5042-726964675854  notify            (board -> phone)
//     CTRL 0347686F-7374-4553-5042-72694C525443  write / write-NR  (also accepts cmds)
//     OTA  0447686F-7374-4553-5042-72694F544144  write-NR          (firmware image; stubbed)
//   Name in scan response: "iosESP-XXXX" (last two bytes of the BLE address).
//   Input: unframed ASCII commands are accepted (we also unwrap the 12-byte
//          GB frame if present). Output: text WD: lines wrapped in DATA frames.
//
// This mirrors the BiscuitCompat module so BLEBridge can run Wardrive Go
// across Solo, Mesh Node, and Mesh Core modes.

// Primary GATT service UUID advertised to Wardrive Go.
const char* iosEspCompatServiceUuid();

// Build and return the "iosESP-XXXX" GAP/scan-response name from BLE MAC.
// Call after NimBLEDevice::init().
const char* iosEspCompatBuildName();

// Create the iosESP service + characteristics on an existing server.
void iosEspCompatAttach(NimBLEServer* server);
// Stand up BLE on our own (standalone), advertising as iosESP-XXXX.
void iosEspCompatBeginStandalone(const char* fallbackName = nullptr);
// Tear BLE back down (frees the controller for ESP-NOW / WiFi churn).
void iosEspCompatStopStandalone(bool freeStack = true);
bool iosEspCompatActive();
bool iosEspCompatStreaming();
// Detach without touching NimBLE (caller owns teardown; for the bridge tree).
void iosEspCompatDetach();
// Notify the module that the central disconnected on a shared NimBLEServer.
void iosEspCompatOnDisconnect();

// Main-loop service: drains inbound commands, streams queued WD: lines,
// emits the periodic WD:STATUS heartbeat.
void iosEspCompatTick();

// Forward one scanned AP. `auth` is the WiGLE/JCMK capabilities string
// (e.g. "[WPA2-PSK-CCMP][ESS]" or "WPA2"); it is mapped to a WD auth token.
void iosEspCompatPushAp(const char* bssid, const char* ssid,
                        int channel, int rssi, const char* auth);
// Forward a GPS fix -> WD:GPS. `sats` = satellites in use (0 if unknown).
void iosEspCompatPushGps(double lat, double lon, double altMeters, int sats);

// Report whether a GPS module is ATTACHED, independent of whether it has a fix
// yet (drive it from e.g. `gps.charsProcessed() > 0`). WD:CAPS carries this as
// gps=0/1, and the handshake that contains it is sent once, at the moment the
// phone subscribes — so without this the app is told gps=0 for the whole
// session whenever it connects before the first fix. Changing the value while a
// phone is connected re-sends WD:CAPS alone, which the app accepts at any time
// (unlike WD:BEGIN, which must never be replayed). Optional: the first
// iosEspCompatPushGps() also flips it on.
void iosEspCompatSetGpsPresent(bool present);

// True when a connected, streaming Wardrive Go phone has narrowed the channel
// plan below every supported channel (via `-ch` / `ch`). Fills `out` (capacity
// >= 48) with that channel list and `outCount` with its length, so the Solo
// scan loop can scan only those channels instead of sweeping all. False when no
// phone is streaming or the plan is still every channel.
bool iosEspCompatChannelOverride(uint8_t* out, uint8_t* outCount);

