#include "BLEBridge.h"
#include "Globals.h"      // cfg, gps, gpsHasFix, networksFound2G/5G, FIRMWARE_VERSION
#include "MeshNode.h"     // meshCoreActive, coreRecordsRx, coreNodeCount, coreNodes[], JCMK_CHANNELS
#include "BiscuitCompat.h"// present as a Biscuit Pro to the Biscuit Manager app (rides this server)
#include "IosEspCompat.h" // present as an iosESP bridge to the Wardrive Go app (rides this server)

#include <NimBLEDevice.h>

// ================================================================
//  GATT UUIDs  (base 7A1F00NN-9B7E-4C2A-9E1D-9A6F3C5B1E00)
// ================================================================
#define SVC_UUID      "7A1F0000-9B7E-4C2A-9E1D-9A6F3C5B1E00"
#define CH_STATUS     "7A1F0001-9B7E-4C2A-9E1D-9A6F3C5B1E00"
#define CH_RECORD     "7A1F0002-9B7E-4C2A-9E1D-9A6F3C5B1E00"
#define CH_NODES      "7A1F0003-9B7E-4C2A-9E1D-9A6F3C5B1E00"
#define CH_CONTROL    "7A1F0004-9B7E-4C2A-9E1D-9A6F3C5B1E00"

// ================================================================
//  Module state
// ================================================================
static bool                  bridgeUp      = false;
static NimBLEServer*         bleServer     = nullptr;
static NimBLECharacteristic* chStatus      = nullptr;
static NimBLECharacteristic* chRecord      = nullptr;
static NimBLECharacteristic* chNodes       = nullptr;
static NimBLECharacteristic* chControl     = nullptr;
static volatile uint16_t     connectedCount = 0;

volatile bool bleBridgeExitRequested = false;

// Pending control commands (set in BLE callback, applied in tick).
static volatile bool pendingClearStats = false;
static volatile bool pendingPing       = false;

// Record ring buffer. Entries are pre-formatted JSON so the tick just
// setValue()+notify()s them. 224 bytes holds the largest realistic record
// (32-char escaped SSID + long auth string + GPS + timestamp).
#define REC_SLOT_LEN 224
#define REC_RING     16
struct RecSlot { char json[REC_SLOT_LEN]; };
static RecSlot  recRing[REC_RING];
static volatile uint8_t recHead = 0;   // drained by tick
static volatile uint8_t recTail = 0;   // filled by push

// Roster-change detection + Status cadence.
static uint8_t  rosterVer       = 0;
static uint32_t lastRosterSig   = 0xFFFFFFFF;
static uint32_t lastStatusMs    = 0;
static const uint32_t STATUS_PERIOD_MS = 1000;

// ================================================================
//  JSON escaping (SSIDs can contain " \ and control chars)
// ================================================================
static void jsonEscapeInto(String& out, const char* s) {
  for (const char* p = s; *p; ++p) {
    unsigned char c = (unsigned char)*p;
    switch (c) {
      case '"':  out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n";  break;
      case '\r': out += "\\r";  break;
      case '\t': out += "\\t";  break;
      default:
        if (c < 0x20) { char b[7]; snprintf(b, sizeof(b), "\\u%04x", c); out += b; }
        else          { out += (char)c; }
    }
  }
}

static void macToStr(const uint8_t* m, char* out /*>=18*/) {
  snprintf(out, 18, "%02X:%02X:%02X:%02X:%02X:%02X",
           m[0], m[1], m[2], m[3], m[4], m[5]);
}

// ================================================================
//  Callbacks
//
//  NimBLE-Arduino v2.x signatures (the version this firmware builds
//  against on the Arduino-ESP32 3.x / IDF 5 core). v2.x adds a
//  `NimBLEConnInfo& connInfo` parameter to onConnect/onDisconnect/onWrite
//  and gives onDisconnect a trailing `int reason` — everything else is
//  identical. To build against the old v1.4.x API, drop those extra
//  parameters.
// ================================================================
class BridgeServerCB : public NimBLEServerCallbacks {
  void onConnect(NimBLEServer* s, NimBLEConnInfo& connInfo) override {
    connectedCount++;
    if (cfg.blePersonality == "biscuit") {
      // Keep advertising so a second central (or reconnect) can still find us.
      NimBLEDevice::startAdvertising();
    } else {
      // Wardrive Go treats continued advertisements while connected as a stale
      // link and terminates the connection (reason 531 / 0x13). Stop advertising
      // while connected; onDisconnect re-arms it immediately.
      NimBLEDevice::stopAdvertising();
    }
  }
  void onDisconnect(NimBLEServer* s, NimBLEConnInfo& connInfo, int reason) override {
    if (connectedCount) connectedCount--;
    iosEspCompatOnDisconnect();
    NimBLEDevice::startAdvertising();
  }
};

class ControlCB : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic* c, NimBLEConnInfo& connInfo) override {
    // Do NO real work here — just classify and flag. Applied in tick().
    std::string v = c->getValue();          // NimBLEAttValue, converts to std::string
    if (v.empty()) return;
    if      (v.find("clearStats") != std::string::npos) pendingClearStats = true;
    else if (v.find("exitMesh")   != std::string::npos) bleBridgeExitRequested = true;
    else if (v.find("ping")       != std::string::npos) pendingPing = true;
  }
};

static BridgeServerCB serverCb;
static ControlCB      controlCb;

// ================================================================
//  Lifecycle
// ================================================================
void bleBridgeBegin() {
  if (bridgeUp) return;

  // Advertised name for Biscuit / WarHogMesh: "WarHog-Core" or "WarHog-Core <deviceName>".
  String name = "WarHog-Core";
  if (cfg.deviceName.length()) { name += " "; name += cfg.deviceName; }

  if (!NimBLEDevice::init(std::string(name.c_str()))) {
    Serial.println("[BLE] NimBLEDevice::init failed");
    return;
  }
  NimBLEDevice::setMTU(247);                  // request a larger ATT MTU
  NimBLEDevice::setPower(9);                   // +9 dBm — max TX power (v2.x takes dBm int)

  bleServer = NimBLEDevice::createServer();
  if (!bleServer) {
    Serial.println("[BLE] createServer failed");
    return;
  }
  // false => don't let deinit(true) delete this static callback (it would
  // free() a non-heap pointer and assert on BLE teardown / mesh exit).
  bleServer->setCallbacks(&serverCb, false);

  NimBLEService* svc = bleServer->createService(SVC_UUID);
  chStatus  = svc->createCharacteristic(CH_STATUS,
                NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY);
  chRecord  = svc->createCharacteristic(CH_RECORD,
                NIMBLE_PROPERTY::NOTIFY);
  chNodes   = svc->createCharacteristic(CH_NODES,
                NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY);
  chControl = svc->createCharacteristic(CH_CONTROL,
                NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR);
  chControl->setCallbacks(&controlCb);

  chStatus->setValue("{\"v\":1,\"core\":true}");
  chNodes->setValue("{\"v\":1,\"nodes\":[]}");

  svc->start();

  // Add both the Wardrive Go (iosESP) and Biscuit Pro services onto this same
  // server so either companion app can use the GATT table. Must be before
  // advertising.
  biscuitCompatAttach(bleServer);
  iosEspCompatAttach(bleServer);

  NimBLEAdvertising* adv = NimBLEDevice::getAdvertising();
  if (cfg.blePersonality == "biscuit") {
    // Advertise BOTH Biscuit + WarHogMesh services. Two 128-bit UUIDs won't fit
    // one 31-byte packet, so the Biscuit UUID goes in the primary advertisement
    // and the mesh UUID + name go in the scan response.
    adv->addServiceUUID(BISCUIT_SERVICE_UUID);

    NimBLEAdvertisementData scanResp;
    scanResp.setName(std::string(name.c_str()));
    scanResp.addServiceUUID(SVC_UUID);
    adv->enableScanResponse(true);              // v2.x: was setScanResponse(true) in v1.4.x
    adv->setScanResponseData(scanResp);
  } else {
    // Default ("wardrivego"): advertise as "iosESP-XXXX" with the iosESP GATT
    // service UUID in the primary advertisement and the GAP name in the scan
    // response so the Wardrive Go / WarBLE Go app pairs and streams in all modes.
    const char* ieName = iosEspCompatBuildName();
    name = ieName;
    NimBLEDevice::setDeviceName(ieName);

    NimBLEAdvertisementData advData;
    advData.setFlags(0x06);
    advData.addServiceUUID(NimBLEUUID(iosEspCompatServiceUuid()));
    adv->setAdvertisementData(advData);

    NimBLEAdvertisementData scanResp;
    scanResp.setName(ieName);
    adv->enableScanResponse(true);
    adv->setScanResponseData(scanResp);
  }
  NimBLEDevice::startAdvertising();

  recHead = recTail = 0;
  rosterVer = 0;
  lastRosterSig = 0xFFFFFFFF;
  lastStatusMs = 0;
  connectedCount = 0;
  bridgeUp = true;
  Serial.printf("[BLE] Bridge up as \"%s\" (personality=%s)\n",
                name.c_str(), cfg.blePersonality.c_str());
}

void bleBridgeStop() {
  if (!bridgeUp) return;
  bridgeUp = false;
  biscuitCompatDetach();          // drop Biscuit refs before we free the controller
  iosEspCompatDetach();           // drop Wardrive Go refs before we free the controller
  NimBLEDevice::stopAdvertising();
  // Full deinit frees the BT controller RAM for the WiFi/ESP-Now path.
  NimBLEDevice::deinit(true);
  bleServer = nullptr;
  chStatus = chRecord = chNodes = chControl = nullptr;
  connectedCount = 0;
  Serial.println("[BLE] Bridge down");
}

bool bleBridgeActive() { return bridgeUp; }

// ================================================================
//  Record enqueue (main-loop safe; no BLE calls)
// ================================================================
void bleBridgePushRecord(const char* bssid, const char* ssid, const char* auth,
                         int ch, int rssi,
                         double lat, double lon, double alt, double acc,
                         const char* isoTime) {
  if (!bridgeUp) return;

  // Mirror each AP to both Wardrive Go and Biscuit Manager so whichever app is
  // subscribed plots it in real time on the map.
  iosEspCompatPushAp(bssid, ssid, ch, rssi, auth);
  biscuitCompatPushAp(bssid, ssid, ch, rssi, auth);

  uint8_t next = (recTail + 1) % REC_RING;
  if (next == recHead) return;   // ring full — drop (SD CSV still has it)

  String j; j.reserve(REC_SLOT_LEN);
  j += "{\"b\":\"";  jsonEscapeInto(j, bssid ? bssid : "");
  j += "\",\"s\":\""; jsonEscapeInto(j, ssid  ? ssid  : "");
  j += "\",\"a\":\""; jsonEscapeInto(j, auth  ? auth  : "");
  char tail[160];
  snprintf(tail, sizeof(tail),
           "\",\"c\":%d,\"r\":%d,\"lat\":%.6f,\"lon\":%.6f,\"alt\":%.1f,\"acc\":%.2f,\"t\":\"%s\"}",
           ch, rssi, lat, lon, alt, acc, isoTime ? isoTime : "");
  j += tail;

  strncpy(recRing[recTail].json, j.c_str(), REC_SLOT_LEN - 1);
  recRing[recTail].json[REC_SLOT_LEN - 1] = '\0';
  recTail = next;
}

// ================================================================
//  Roster / status builders
// ================================================================
static uint32_t computeRosterSig() {
  // Changes on membership and channel-assignment changes, but NOT on every
  // record (recordsRx is excluded) — so the app only re-reads Nodes when the
  // roster actually changes shape.
  uint32_t sig = coreNodeCount * 2654435761u;
  for (uint8_t i = 0; i < CORE_MAX_NODES; i++) {
    if (!coreNodes[i].active) continue;
    for (uint8_t b = 0; b < 6; b++) sig = sig * 31u + coreNodes[i].mac[b];
    sig = sig * 31u + coreNodes[i].startIdx;
    sig = sig * 31u + coreNodes[i].endIdx;
    sig = sig * 31u + coreNodes[i].slotIndex;
  }
  return sig;
}

static void buildNodesJson(String& out) {
  out = "{\"v\":1,\"rosterVer\":";
  out += rosterVer;
  out += ",\"nodes\":[";
  uint32_t now = millis();
  bool first = true;
  for (uint8_t i = 0; i < CORE_MAX_NODES; i++) {
    if (!coreNodes[i].active) continue;
    if (!first) out += ",";
    first = false;
    char mac[18]; macToStr(coreNodes[i].mac, mac);
    const char* kind = coreNodes[i].isWarHog ? "warhog"
                     : (coreNodes[i].isBiscuit ? "biscuit" : "jcmk");
    char buf[160];
    snprintf(buf, sizeof(buf),
      "{\"mac\":\"%s\",\"kind\":\"%s\",\"rx\":%lu,\"ch0\":%u,\"ch1\":%u,\"slot\":%u,\"ageMs\":%lu}",
      mac, kind, (unsigned long)coreNodes[i].recordsRx,
      coreNodes[i].startIdx, coreNodes[i].endIdx, coreNodes[i].slotIndex,
      (unsigned long)(now - coreNodes[i].lastHbMs));
    out += buf;
  }
  out += "]}";
}

static void buildStatusJson(String& out) {
  double lat = 0, lon = 0, alt = 0, acc = 0;
  if (gpsHasFix) {
    lat = gps.location.lat();  lon = gps.location.lng();
    alt = gps.altitude.meters(); acc = gps.hdop.hdop();
  }
  char buf[320];
  snprintf(buf, sizeof(buf),
    "{\"v\":%d,\"fw\":\"%s\",\"core\":%s,\"rx\":%lu,\"nodes\":%u,\"rosterVer\":%u,"
    "\"found2g\":%lu,\"found5g\":%lu,\"fix\":%s,"
    "\"lat\":%.6f,\"lon\":%.6f,\"alt\":%.1f,\"acc\":%.2f,\"upMs\":%lu}",
    BLE_BRIDGE_PROTO_VER, FIRMWARE_VERSION,
    meshCoreActive ? "true" : "false",
    (unsigned long)coreRecordsRx, coreNodeCount, rosterVer,
    (unsigned long)networksFound2G, (unsigned long)networksFound5G,
    gpsHasFix ? "true" : "false",
    lat, lon, alt, acc, (unsigned long)millis());
  out = buf;
}

// ================================================================
//  Main-loop tick
// ================================================================
void bleBridgeTick() {
  if (!bridgeUp) return;

  // 0. Service Wardrive Go (iosESP) + Biscuit Manager commands and stream
  //    telemetry; push GPS ~1 Hz so the app can pin the mirrored APs to our
  //    location.
  iosEspCompatSetGpsPresent(gps.charsProcessed() > 0);
  iosEspCompatTick();
  biscuitCompatTick();
  {
    static uint32_t biscuitGpsMs = 0;
    uint32_t nowg = millis();
    if (gpsHasFix && nowg - biscuitGpsMs >= 1000) {
      biscuitGpsMs = nowg;
      int sats = gps.satellites.isValid() ? (int)gps.satellites.value() : 0;
      iosEspCompatPushGps(gps.location.lat(), gps.location.lng(),
                          gps.altitude.meters(), sats);
      biscuitCompatPushGps(gps.location.lat(), gps.location.lng(),
                           gps.altitude.meters(), gps.hdop.hdop());
    }
  }

  // 1. Apply pending control commands (safe: we're in the main loop).
  if (pendingClearStats) {
    pendingClearStats = false;
    coreRecordsRx = 0;
    for (uint8_t i = 0; i < CORE_MAX_NODES; i++)
      if (coreNodes[i].active) coreNodes[i].recordsRx = 0;
  }
  if (pendingPing) { pendingPing = false; lastStatusMs = 0; /* force a status push */ }

  bool haveClient = (connectedCount > 0);

  // 2. Drain the record ring → one notification per record.
  while (recHead != recTail) {
    uint8_t i = recHead;
    recHead = (recHead + 1) % REC_RING;
    if (haveClient && chRecord) {
      chRecord->setValue((uint8_t*)recRing[i].json, strlen(recRing[i].json));
      chRecord->notify();
    }
  }

  // 3. Roster change → rebuild Nodes value, bump version, notify (best-effort).
  uint32_t sig = computeRosterSig();
  if (sig != lastRosterSig) {
    lastRosterSig = sig;
    rosterVer++;
    String nodes; buildNodesJson(nodes);
    if (chNodes) {
      chNodes->setValue((uint8_t*)nodes.c_str(), nodes.length());
      if (haveClient) chNodes->notify();   // app treats as a poke; re-reads if truncated
    }
  }

  // 4. Periodic Status (also refreshes the readable value even with no client).
  uint32_t now = millis();
  if (now - lastStatusMs >= STATUS_PERIOD_MS) {
    lastStatusMs = now;
    String st; buildStatusJson(st);
    if (chStatus) {
      chStatus->setValue((uint8_t*)st.c_str(), st.length());
      if (haveClient) chStatus->notify();
    }
  }
}
