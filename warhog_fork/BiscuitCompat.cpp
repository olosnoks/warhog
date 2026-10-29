#include "BiscuitCompat.h"

// Reported in DIS Software Revision (0x2A28). Not gate-critical; if the real
// build version isn't visible in this translation unit, fall back gracefully.
#ifndef FIRMWARE_VERSION
#define FIRMWARE_VERSION "warhog"
#endif

// ================================================================
//  Biscuit GATT UUIDs (from Biscuit Manager 1.0.21.1)
// ================================================================
const char* BISCUIT_SERVICE_UUID = "4FAFC201-1FB5-459E-8FCC-C5C9C331914B";
#define CH_COMMAND   "BEB5483E-36E1-4688-B7F5-EA07361B26A8"   // write
#define CH_RESPONSE  "BEB5483E-36E1-4688-B7F5-EA07361B26A9"   // notify
#define CH_STATUS    "BEB5483E-36E1-4688-B7F5-EA07361B26AA"   // notify
#define CH_SETTINGS  "BEB5483E-36E1-4688-B7F5-EA07361B26AB"   // read
#define CH_SECURITY  "BEB5483E-36E1-4688-B7F5-EA07361B26AC"   // read

// Standard SIG services/chars
#define DIS_SERVICE  "180A"
#define DIS_MODEL    "2A24"   // Model Number  -> tier switch
#define DIS_SERIAL   "2A25"
#define DIS_FWREV    "2A26"   // Firmware Revision -> HID gate
#define DIS_SWREV    "2A28"
#define DIS_MFR      "2A29"

// ================================================================
//  Module state
// ================================================================
static bool                  bcActive     = false;
static bool                  bcStandalone = false;
static NimBLECharacteristic* chResponse   = nullptr;
static NimBLECharacteristic* chStatus     = nullptr;
static NimBLEServer*         bcServer     = nullptr;

// Inbound command ring (write callback enqueues; tick drains/replies).
#define CMD_SLOT_LEN 128
#define CMD_RING     8
struct CmdSlot { char line[CMD_SLOT_LEN]; };
static CmdSlot           cmdRing[CMD_RING];
static volatile uint8_t  cmdHead = 0;   // drained by tick
static volatile uint8_t  cmdTail = 0;   // filled by write cb

// Outbound telemetry ring (record path enqueues DATA:...; tick notifies).
// Keep this modest: it's static DRAM, and an oversized buffer starves the heap
// enough that lazy ADC init (battery monitor) can't allocate and abort()s.
// Throughput comes from draining fast (TX_PER_TICK), not from a huge ring —
// the app dedups by BSSID, so a lossy stream of re-sightings still converges.
#define TX_SLOT_LEN  128
#define TX_RING      48          // 96*128 = ~12 KB static (vs original 64*150 = 9.6 KB)
#define TX_PER_TICK  48          // notifies per tick; drains the ring quickly
struct TxSlot { char line[TX_SLOT_LEN]; };
static TxSlot            txRing[TX_RING];
static volatile uint16_t txHead = 0;
static volatile uint16_t txTail = 0;
static volatile bool     respSubscribed = false;   // app enabled Response notifications

// Diagnostics (printed ~3 s while subscribed) so we can see enqueue vs drop vs send.
static volatile uint32_t dbgEnq = 0, dbgDropFull = 0, dbgSent = 0;

// Node-snapshot request flag (set by the Command handler on CMD:getnodesnapshot)
// and a rolling snapshot id (ushort, wraps) so each snapshot is distinct.
static volatile bool     snapshotRequested = false;
static uint16_t          snapshotIdCtr     = 0;
static uint16_t          snapshotLastCount = 0;   // record count of the in-flight snapshot (for the completion line)

static void txEnqueue(const char* line, size_t n) {
  if (!bcActive || !respSubscribed) return;
  uint16_t next = (txTail + 1) % TX_RING;
  if (next == txHead) { dbgDropFull++; return; }   // ring full — drop
  if (n >= TX_SLOT_LEN) n = TX_SLOT_LEN - 1;
  memcpy(txRing[txTail].line, line, n);
  txRing[txTail].line[n] = '\0';
  txTail = next;
  dbgEnq++;
}

// ================================================================
//  Callbacks
// ================================================================
class BiscuitCommandCB : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic* c, NimBLEConnInfo& connInfo) override {
    // Enqueue only — no BLE work in the callback (mirrors BLEBridge).
    std::string v = c->getValue();
    if (v.empty()) return;
    uint8_t next = (cmdTail + 1) % CMD_RING;
    if (next == cmdHead) return;             // ring full — drop
    size_t n = v.size(); if (n >= CMD_SLOT_LEN) n = CMD_SLOT_LEN - 1;
    memcpy(cmdRing[cmdTail].line, v.data(), n);
    cmdRing[cmdTail].line[n] = '\0';
    cmdTail = next;
  }
};

// Standalone-only: keep advertising after a central connects/disconnects so
// the app can reconnect. (In the Arduino tree the bridge's own server
// callbacks already do this.)
class BiscuitStandaloneServerCB : public NimBLEServerCallbacks {
  void onConnect(NimBLEServer* s, NimBLEConnInfo& i) override    { NimBLEDevice::startAdvertising(); }
  void onDisconnect(NimBLEServer* s, NimBLEConnInfo& i, int r) override { NimBLEDevice::startAdvertising(); }
};

// Track whether the app has enabled notifications on the Response char, so the
// hot record path only builds/sends telemetry when someone is actually listening.
class BiscuitResponseCB : public NimBLECharacteristicCallbacks {
  void onSubscribe(NimBLECharacteristic* c, NimBLEConnInfo& connInfo, uint16_t subValue) override {
    respSubscribed = (subValue != 0);
  }
};

static BiscuitCommandCB          commandCb;
static BiscuitResponseCB         responseCb;
static BiscuitStandaloneServerCB standaloneServerCb;

// ================================================================
//  Service construction (shared by both paths)
// ================================================================
void biscuitCompatAttach(NimBLEServer* server) {
  if (!server) return;
  bcServer = server;

  // --- Biscuit control service ---
  NimBLEService* svc = server->createService(BISCUIT_SERVICE_UUID);
  svc->createCharacteristic(CH_COMMAND, NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR)
     ->setCallbacks(&commandCb);
  chResponse = svc->createCharacteristic(CH_RESPONSE, NIMBLE_PROPERTY::NOTIFY);
  chResponse->setCallbacks(&responseCb);     // track Response-notify subscription
  respSubscribed = false;
  txHead = txTail = 0;
  chStatus   = svc->createCharacteristic(CH_STATUS,   NIMBLE_PROPERTY::NOTIFY);
  // Readable Settings/Security so the app's connect-time reads succeed and the
  // link stays Open (no app-layer secret; BLE bonding stays optional).
  svc->createCharacteristic(CH_SETTINGS, NIMBLE_PROPERTY::READ)->setValue("{}");
  svc->createCharacteristic(CH_SECURITY, NIMBLE_PROPERTY::READ)->setValue("OPEN");
  svc->start();

  // --- Device Information: this is what classifies us as a Pro ---
  NimBLEService* dis = server->createService(DIS_SERVICE);
  dis->createCharacteristic(DIS_MODEL,  NIMBLE_PROPERTY::READ)->setValue(BISCUIT_MODEL_STRING);
  // 2A26 = WROOM firmware, 2A28 = ESP32-C5 firmware (the app reads both and
  // shows "N/A" + an inactive scanner if the C5 one isn't a real version).
  dis->createCharacteristic(DIS_FWREV,  NIMBLE_PROPERTY::READ)->setValue(BISCUIT_FW_REVISION);
  dis->createCharacteristic(DIS_SWREV,  NIMBLE_PROPERTY::READ)->setValue(BISCUIT_FW_REVISION);
  dis->createCharacteristic(DIS_MFR,    NIMBLE_PROPERTY::READ)->setValue("WarHog");
  dis->createCharacteristic(DIS_SERIAL, NIMBLE_PROPERTY::READ)->setValue("WARHOG-0001");
  dis->start();

  bcActive = true;
}

// ================================================================
//  Standalone lifecycle (S3 tree)
// ================================================================
void biscuitCompatBeginStandalone(const char* gapName) {
  if (bcActive) return;
  if (!NimBLEDevice::isInitialized())
    NimBLEDevice::init(gapName ? gapName : "WarHog");
  else
    NimBLEDevice::setDeviceName(gapName ? gapName : "WarHog");
  NimBLEDevice::setMTU(247);
  NimBLEDevice::setPower(9);

  NimBLEServer* server = NimBLEDevice::createServer();
  // false => NimBLE must NOT delete this static on deinit(true); otherwise the
  // server destructor free()s a non-heap pointer and asserts. (Char callbacks
  // are not owned/deleted by NimBLE, so commandCb needs no such flag.)
  server->setCallbacks(&standaloneServerCb, false);
  biscuitCompatAttach(server);
  bcStandalone = true;

  NimBLEAdvertising* adv = NimBLEDevice::getAdvertising();
  adv->addServiceUUID(BISCUIT_SERVICE_UUID);   // REQUIRED: app's scan filter
  adv->setName(gapName ? gapName : "WarHog");
  adv->enableScanResponse(true);
  NimBLEDevice::startAdvertising();
  Serial.printf("[BISCUIT] Standalone up as \"%s\" (model=%s)\n",
                gapName ? gapName : "WarHog", BISCUIT_MODEL_STRING);
}

void biscuitCompatStopStandalone() {
  if (!bcActive) return;
  bcActive = false;
  if (bcStandalone) {
    NimBLEDevice::stopAdvertising();
    NimBLEDevice::deinit(true);   // free BT controller RAM for ESP-NOW
    bcStandalone = false;
  }
  respSubscribed = false;
  chResponse = nullptr;
  chStatus   = nullptr;
  bcServer   = nullptr;
  cmdHead = cmdTail = 0;
  txHead  = txTail  = 0;
  Serial.println("[BISCUIT] Standalone down");
}

bool biscuitCompatActive() { return bcActive; }

void biscuitCompatDetach() {
  // No NimBLE calls — the caller (bridge) owns teardown/deinit.
  bcActive = false;
  bcStandalone = false;
  respSubscribed = false;
  chResponse = nullptr;
  chStatus   = nullptr;
  bcServer   = nullptr;
  cmdHead = cmdTail = 0;
  txHead  = txTail  = 0;
}

// ================================================================
//  Command handling (minimal identify build)
//
//  Returns a reply line to notify on the Response characteristic, or
//  empty for no reply. This is the single place to extend later:
//  map CMD:scanap:/sniffpmkid:/beaconspam:/... to real WarHog calls
//  and emit DATA:<TYPE>:<payload> telemetry via chStatus/chResponse.
// ================================================================
static String biscuitHandleCommand(const String& raw) {
  // Split TYPE:SUBTYPE:PAYLOAD (max 3).
  int p1 = raw.indexOf(':');
  String type = (p1 < 0) ? raw : raw.substring(0, p1);
  String rest = (p1 < 0) ? ""  : raw.substring(p1 + 1);
  int p2 = rest.indexOf(':');
  String sub = (p2 < 0) ? rest : rest.substring(0, p2);
  type.trim(); sub.trim();

  if (type == "ACK" || type == "TIME") return "";   // accept silently

  if (type == "CMD") {
    // Node-manager queries the Pro UI fires on connect: ack + empty data so
    // the manager view populates without error.
    if (sub == "getnodelist")     { return "RSP:getnodelist:OK"; }
    if (sub == "getnodeconfig") {
      // Clear the app's "Loading settings…" by reporting a manager config.
      // mode=manager marks us as the authoritative node manager; the live
      // roster itself is delivered by the snapshot stream, so knownNodes=[].
      const char* cfg = "RSP:nodeconfig:{\"enabled\":true,\"key\":\"\","
                        "\"encrypt\":true,\"maxNodes\":6,\"mode\":\"manager\","
                        "\"registryCapacity\":30,\"knownNodes\":[]}";
      txEnqueue(cfg, strlen(cfg));
      return "RSP:getnodeconfig:OK";
    }
    if (sub == "getnodesnapshot") { snapshotRequested = true; return "RSP:getnodesnapshot:OK"; }
    if (sub == "info")            { return "RSP:info:OK"; }
    // Everything else: accept so the app doesn't show an error.
    return String("RSP:") + sub + ":OK";
  }
  return "";
}

// ================================================================
//  Live telemetry push (connected + subscribed gated; hot-path safe)
// ================================================================
void biscuitCompatPushAp(const char* bssid, const char* ssid,
                         int channel, int rssi, const char* authMode) {
  if (!bcActive || !respSubscribed) return;
  if (channel < 1) return;                 // ch 0 => BLE/unknown, not an AP
  if (rssi < -128) rssi = -128; else if (rssi > 127) rssi = 127;

  // SSID must not contain commas (the app splits on ',' with no limit).
  String sid = ssid ? ssid : "";
  sid.replace(",", " ");
  // authMode must be WiGLE bracket form or the app rejects the row.
  const char* a = (authMode && authMode[0] == '[') ? authMode : "[OPEN][ESS]";

  char line[TX_SLOT_LEN];
  int n = snprintf(line, sizeof(line), "DATA:AP:%s,%s,%d,%d,%s",
                   bssid ? bssid : "", sid.c_str(), channel, rssi, a);
  if (n > 0) txEnqueue(line, (size_t)n);
}

void biscuitCompatPushGps(double lat, double lon, double altMeters, double accuracy) {
  if (!bcActive || !respSubscribed) return;
  char line[TX_SLOT_LEN];
  int n = snprintf(line, sizeof(line), "DATA:GPS:%.6f,%.6f,%.1f,%.2f",
                   lat, lon, altMeters, accuracy);
  if (n > 0) txEnqueue(line, (size_t)n);
}

// ================================================================
//  Node roster snapshot emitters
// ================================================================
bool biscuitCompatConsumeSnapshotRequest() {
  if (!snapshotRequested) return false;
  snapshotRequested = false;
  return true;
}

uint16_t biscuitCompatSnapshotBegin(uint16_t nodeCount) {
  if (nodeCount > 30) nodeCount = 30;              // registry capacity is fixed at 30
  snapshotLastCount = nodeCount;                   // remember for the completion's recordCount field
  uint16_t id = snapshotIdCtr++;                   // ushort, wraps naturally
  if (!bcActive || !respSubscribed) return id;
  // version=1, enabled=1, encrypt=1 (must equal maxActive<=6), maxActive=6,
  // mode=manager, registryCapacity=30, known=count=nodeCount.
  char line[TX_SLOT_LEN];
  int n = snprintf(line, sizeof(line),
                   "DATA:NODESNAPSHOT:1,%u,1,1,6,manager,30,%u,%u",
                   id, nodeCount, nodeCount);
  if (n > 0) { txEnqueue(line, (size_t)n); Serial.print("[SNAP] "); Serial.println(line); }
  return id;
}

void biscuitCompatSnapshotRecord(uint16_t snapId, uint16_t index,
                                 const char* macUpper, int scanCount,
                                 int rssi, int nodeType) {
  if (!bcActive || !respSubscribed) return;
  if (scanCount < 0) scanCount = 0;
  if (rssi < -127) rssi = -127; else if (rssi > 20) rssi = 20;
  if (nodeType < 1 || nodeType > 3) nodeType = 1;  // default C5/biscuit
  // saved=1, state=1(CONNECTED), role=0, firmware="" (empty, valid), caps=0.
  char line[TX_SLOT_LEN];
  int n = snprintf(line, sizeof(line),
                   "DATA:NODESTATE:%u,%u,1,%s,1,%d,%d,0,,%d,0",
                   snapId, index, macUpper ? macUpper : "00:00:00:00:00:00",
                   rssi, scanCount, nodeType);
  if (n > 0) { txEnqueue(line, (size_t)n); Serial.print("[SNAP] "); Serial.println(line); }
}

void biscuitCompatSnapshotComplete(uint16_t snapId) {
  if (!bcActive || !respSubscribed) return;
  char line[TX_SLOT_LEN];
  // Completion MUST be 3 fields: id,OK,recordCount. The app's
  // NodeSnapshotAccumulator.finish() rejects anything else ("Invalid snapshot
  // completion status"), which both times out the roster AND keeps the
  // accumulator perpetually "staging" so getnodeconfig is ignored too.
  int n = snprintf(line, sizeof(line), "RSP:nodesnapshot:%u,OK,%u", snapId, snapshotLastCount);
  if (n > 0) { txEnqueue(line, (size_t)n); Serial.print("[SNAP] "); Serial.println(line); }
}

// ================================================================
//  Main-loop tick
// ================================================================
void biscuitCompatTick() {
  if (!bcActive) return;

  // 1. Answer inbound commands. Replies ride the SAME outbound ring as telemetry
  // instead of being notified from here: a bare notify() is silently discarded
  // when the controller is out of buffers, and a lost RSP: line leaves the app
  // spinning forever. The drain below handles both with backpressure, and the
  // ring also fixes the ordering of CMD:getnodeconfig — its config blob is
  // enqueued by the handler, so it now goes out ahead of its own RSP:...:OK.
  while (cmdHead != cmdTail) {
    uint8_t i = cmdHead;
    cmdHead = (cmdHead + 1) % CMD_RING;
    String reply = biscuitHandleCommand(String(cmdRing[i].line));
    if (reply.length()) txEnqueue(reply.c_str(), reply.length());
  }

  // 2. Stream queued replies + telemetry (RSP: / DATA:AP / DATA:GPS), capped per
  // tick and WITH BACKPRESSURE. NimBLE's notify() returns false when the
  // controller is out of buffers, and every notification past that point is
  // silently dropped — not queued. So advance the ring ONLY on an accepted
  // notify and leave the rest for the next tick. (Telemetry survived this
  // because the app dedups by BSSID and re-sightings keep coming, but a burst
  // of command replies or snapshot lines did not. Same rule as IosEspCompat.)
  if (respSubscribed && chResponse) {
    uint16_t sent = 0;
    while (txHead != txTail && sent < TX_PER_TICK) {
      uint16_t i = txHead;
      if (!chResponse->notify((uint8_t*)txRing[i].line, strlen(txRing[i].line)))
        break;                               // buffers full: retry this slot next tick
      txHead = (txHead + 1) % TX_RING;
      sent++;
    }
    dbgSent += sent;

    static uint32_t dbgMs = 0;
    uint32_t nowd = millis();
    if (nowd - dbgMs >= 3000) {
      dbgMs = nowd;
      uint16_t depth = (txTail - txHead + TX_RING) % TX_RING;
      Serial.printf("[BISCUIT] tx enq=%lu sent=%lu dropFull=%lu ringDepth=%u\n",
                    (unsigned long)dbgEnq, (unsigned long)dbgSent,
                    (unsigned long)dbgDropFull, depth);
    }
  }
}
