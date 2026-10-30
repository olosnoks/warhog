#include "IosEspCompat.h"
#include "BoardPins.h"
#include "Globals.h"     // FIRMWARE_VERSION
#include "Config.h"      // wardriverIsC5()
#include "MeshNode.h"    // JCMK_CHANNELS, JCMK_NUM_CHANNELS, meshCoreActive
#include <stdarg.h>
#include <string.h>
#include <stdlib.h>      // atoi, strtol

// ================================================================
//  iosESP GATT UUIDs (PROTOCOL.md v1.5)
// ================================================================
static const char* IE_SERVICE_UUID = "47686F73-7445-5350-4272-696467655376";
#define IE_CH_RX    "0147686F-7374-4553-5042-726964675852"   // write / write-NR
#define IE_CH_TX    "0247686F-7374-4553-5042-726964675854"   // notify
#define IE_CH_CTRL  "0347686F-7374-4553-5042-72694C525443"   // write / write-NR
#define IE_CH_OTA   "0447686F-7374-4553-5042-72694F544144"   // write-NR (stubbed)

// Chip identity reported in WD:CAPS / info.
#ifdef CONFIG_IDF_TARGET
#define IE_CHIP CONFIG_IDF_TARGET      // "esp32s3", "esp32c5", ...
#else
#define IE_CHIP "esp32"
#endif

// Report Piglet's version as a numeric 1<major>.<minor>.00 (e.g. v2.84 -> "12.84.00")
// so Wardrive Go's number-by-number "outdated firmware" check never fires, while
// still tracking the real FIRMWARE_VERSION for support. Mirrors upstream Piglet's
// wgFwVersion() — this is what makes the app accept the board as current.
static const char* ieFwVersion() {
  static char buf[16] = "";
  if (buf[0]) return buf;
  const char* v = FIRMWARE_VERSION;
  if (*v == 'v' || *v == 'V') v++;
  int major = atoi(v);
  int minor = 0;
  const char* dot = strchr(v, '.');
  if (dot) minor = atoi(dot + 1);
  snprintf(buf, sizeof(buf), "1%d.%02d.00", major, minor);
  return buf;
}

// Frame header (little-endian): GB ver type status flags id[4] len[2]
#define FR_MAGIC_0 0x47
#define FR_MAGIC_1 0x42
#define FR_VERSION 0x01
#define FR_CMD     0x01
#define FR_ACK     0x02
#define FR_DATA    0x03
#define FR_HEADER  12

// ================================================================
//  Module state
// ================================================================
static bool                  ieActive     = false;
static bool                  ieStandalone = false;
static bool                  ieNotifyOn   = false;   // phone subscribed to TX
static bool                  ieStreaming  = false;   // between WD:BEGIN and WD:END
static bool                  ieGpsPresent = false;   // runtime GPS presence-detect
static NimBLECharacteristic* chTx         = nullptr;
static NimBLEServer*         ieServer     = nullptr;
static char                  ieName[16]   = "iosESP";
static uint32_t              ieReplyId    = 0;        // id stamped on DATA frames

// Per-sweep / session counters for WD:STATUS.
static uint32_t ieApSweep   = 0;
static uint32_t ieSweeps    = 0;
static uint32_t ieLastStat  = 0;
static bool     ieRebootReq = false;

// Channel plan reported in WD:CAPS / WD:PLAN and used to filter forwarded APs.
// Supported set is 2.4 GHz only, unless this is a dual-band C5 or we're a Core
// (a Core relays whatever band each node reports, so it is never radio-limited).
// The app can narrow the active plan via `-ch` on `wdstream start` or `ch`.
static uint8_t ieSupportedCount = 0;   // # of leading JCMK_CHANNELS we support
static uint8_t iePlan[48];             // active plan (subset the app asked for)
static uint8_t iePlanCount   = 0;

static bool ieDualBand() { return meshCoreActive || wardriverIsC5(); }

static void ieComputeSupported() {
  const bool dual = ieDualBand();
  uint8_t n = 0;
  for (uint8_t i = 0; i < JCMK_NUM_CHANNELS; i++) {
    if (!dual && JCMK_CHANNELS[i] > 14) break;   // 2.4 GHz prefix only
    n++;
  }
  ieSupportedCount = n;
}
static bool ieChannelSupported(uint8_t ch) {
  for (uint8_t i = 0; i < ieSupportedCount; i++) if (JCMK_CHANNELS[i] == ch) return true;
  return false;
}
static bool ieChannelInPlan(int ch) {
  for (uint8_t i = 0; i < iePlanCount; i++) if (iePlan[i] == (uint8_t)ch) return true;
  return false;
}
static void iePlanResetToAll() {
  ieComputeSupported();
  memcpy(iePlan, JCMK_CHANNELS, ieSupportedCount);
  iePlanCount = ieSupportedCount;
}
static int ieFormatChannelList(char* buf, size_t cap, const uint8_t* ch, uint8_t n) {
  int len = 0;
  for (uint8_t i = 0; i < n && len < (int)cap - 5; i++)
    len += snprintf(buf + len, cap - len, i ? ",%u" : "%u", (unsigned)ch[i]);
  return len;
}
// `-ch 1,6,11` / `ch 1,6,11` / `ch all`: narrow (or reset) the active plan to
// supported channels the app asked for; empty/invalid falls back to all.
static void ieSetChannelPlan(const char* s) {
  while (*s == ' ') s++;
  if (!*s || !strcmp(s, "all")) { iePlanResetToAll(); return; }
  uint8_t tmp[48]; uint8_t n = 0;
  while (*s && n < sizeof(tmp)) {
    while (*s == ',' || *s == ' ') s++;
    if (*s < '0' || *s > '9') break;
    char* end; long v = strtol(s, &end, 10); s = end;
    if (v > 0 && v <= 255 && ieChannelSupported((uint8_t)v)) {
      bool dup = false;
      for (uint8_t i = 0; i < n; i++) if (tmp[i] == (uint8_t)v) dup = true;
      if (!dup) tmp[n++] = (uint8_t)v;
    }
  }
  if (n == 0) { iePlanResetToAll(); return; }
  memcpy(iePlan, tmp, n); iePlanCount = n;
}

// Inbound command ring (write cb enqueues raw bytes; tick drains/replies).
#define IE_CMD_SLOT_LEN 256
#define IE_CMD_RING     8
struct IeCmdSlot { uint8_t data[IE_CMD_SLOT_LEN]; uint16_t len; };
static IeCmdSlot         ieCmdRing[IE_CMD_RING];
static volatile uint8_t  ieCmdHead = 0;
static volatile uint8_t  ieCmdTail = 0;

// Outbound frame ring (binary DATA/ACK frames; tick notifies).
#define IE_TX_SLOT_LEN 200
#define IE_TX_RING     64
#define IE_TX_PER_TICK 48
struct IeTxSlot { uint8_t data[IE_TX_SLOT_LEN]; uint8_t len; };
static IeTxSlot          ieTxRing[IE_TX_RING];
static volatile uint8_t  ieTxHead = 0;
static volatile uint8_t  ieTxTail = 0;

static volatile uint32_t ieEnq = 0, ieDropFull = 0, ieSent = 0;

// ================================================================
//  Frame building / enqueue
// ================================================================
static void ieEnqueueFrame(uint8_t type, uint32_t id, const uint8_t* payload, size_t plen) {
  if (!ieActive || !ieNotifyOn) return;
  if (plen > IE_TX_SLOT_LEN - FR_HEADER) plen = IE_TX_SLOT_LEN - FR_HEADER;
  uint8_t next = (ieTxTail + 1) % IE_TX_RING;
  if (next == ieTxHead) { ieDropFull++; return; }      // ring full -> drop
  uint8_t* f = ieTxRing[ieTxTail].data;
  f[0] = FR_MAGIC_0; f[1] = FR_MAGIC_1; f[2] = FR_VERSION; f[3] = type;
  f[4] = 0; f[5] = 0;
  f[6] = id & 0xff; f[7] = (id >> 8) & 0xff; f[8] = (id >> 16) & 0xff; f[9] = (id >> 24) & 0xff;
  f[10] = plen & 0xff; f[11] = (plen >> 8) & 0xff;
  if (plen) memcpy(f + FR_HEADER, payload, plen);
  ieTxRing[ieTxTail].len = (uint8_t)(FR_HEADER + plen);
  ieTxTail = next;
  ieEnq++;
}

// Emit one WD: text line (newline appended) as a DATA frame stamped with the
// current reply id.
static void ieEmit(const char* fmt, ...) {
  char line[200];
  va_list ap; va_start(ap, fmt);
  int n = vsnprintf(line, sizeof(line) - 1, fmt, ap);
  va_end(ap);
  if (n <= 0) return;
  if (n > (int)sizeof(line) - 2) n = (int)sizeof(line) - 2;
  line[n++] = '\n';
  ieEnqueueFrame(FR_DATA, ieReplyId, (const uint8_t*)line, (size_t)n);
}

// ================================================================
//  WD: output helpers
// ================================================================
static void ieSendCaps() {
  ieComputeSupported();
  char chanBuf[200];
  ieFormatChannelList(chanBuf, sizeof(chanBuf), JCMK_CHANNELS, ieSupportedCount);
  ieEmit("WD:CAPS fw=%s chip=%s bands=%s ota=0 bt=0 gps=%d cap=0 chans=%s",
         ieFwVersion(), IE_CHIP, ieDualBand() ? "2g,5g" : "2g",
         ieGpsPresent ? 1 : 0, chanBuf);
}

// GPS presence is reported in WD:CAPS, which goes out exactly once per stream
// (see ieStartStreaming). If the phone subscribes before the GPS module is
// producing data, that one line claims gps=0 for the rest of the session. So
// when the answer changes under us, re-send WD:CAPS on its own: the app treats
// `caps` as a valid request at any time, and only WD:BEGIN may not be replayed.
void iosEspCompatSetGpsPresent(bool present) {
  if (present == ieGpsPresent) return;
  ieGpsPresent = present;
  if (ieActive && ieNotifyOn) ieSendCaps();
}

// The full WD: scan-start handshake, sent exactly once per stream start.
static void ieSendHandshake() {
  ieSendCaps();
  ieEmit("WD:MODE mode=wifi");
  ieEmit("WD:BEGIN type=wifi interval=0 channel=auto");
  char planBuf[200];
  ieFormatChannelList(planBuf, sizeof(planBuf), iePlan, iePlanCount);
  ieEmit("WD:PLAN chans=%s", planBuf);
}

static void ieStartStreaming() {
  // Already live: just re-confirm the mode. This mirrors the reference firmware,
  // whose set_role() does NOT replay the whole CAPS/BEGIN handshake. Replaying
  // WD:BEGIN makes the app treat it as a stream restart and sit forever on
  // "Starting stream...", so the handshake must be sent exactly once per stream.
  if (ieStreaming) { ieEmit("WD:MODE mode=wifi"); return; }
  ieStreaming = true;
  ieApSweep = 0; ieSweeps = 0; ieLastStat = millis();
  ieSendHandshake();
}

static void ieStopStreaming() {
  if (!ieStreaming) return;
  ieStreaming = false;
  ieEmit("WD:END reason=stop");
}

// Map a WiGLE/JCMK capabilities string to a WD auth token.
static const char* ieAuthToken(const char* auth) {
  if (!auth || !auth[0]) return "OPEN";
  // Case-insensitive substring checks on the capabilities string.
  String a(auth); a.toUpperCase();
  bool w3 = a.indexOf("WPA3") >= 0 || a.indexOf("SAE") >= 0;
  bool w2 = a.indexOf("WPA2") >= 0 || a.indexOf("RSN") >= 0;
  bool w1 = a.indexOf("WPA") >= 0;          // any WPA
  bool wep = a.indexOf("WEP") >= 0;
  if (w3 && w2) return "WPA2_WPA3";
  if (w3)       return "WPA3";
  if (w2)       return "WPA2";
  if (w1)       return "WPA";
  if (wep)      return "WEP";
  // "[ESS]" only / "[OPEN]" / nothing crypto-bearing -> open network.
  if (a.indexOf("OPEN") >= 0) return "OPEN";
  if (a.indexOf("WPA") < 0 && a.indexOf("WEP") < 0) return "OPEN";
  return "UNKNOWN";
}

// ================================================================
//  Command parsing
// ================================================================
static void ieCmdHelp() {
  ieEmit("commands: wdstream start|stop, start, stop, caps, info, status, ch, led on|off, reboot");
}
static void ieCmdInfo() {
  ieEmit("iosESP %s chip=%s name=%s uptime=%lus", ieFwVersion(), IE_CHIP, ieName,
         (unsigned long)(millis() / 1000));
}
static void ieCmdStatus() {
  ieEmit("WD:STATUS aps=%lu scans=%lu", (unsigned long)ieApSweep, (unsigned long)ieSweeps);
}

// `led on|off` from the app: physically identify THIS board (the connected
// bridge) by flashing its only indicator — the display backlight — then
// restoring it. The Freenove S3 CYD drives the backlight as a plain GPIO
// (ScreenUI does digitalWrite on it), so pulsing it here is safe. On the W550
// the backlight is a LovyanGFX PWM channel we must not fight, so it is a no-op
// there. A few hundred ms of blocking in this user-initiated, rare command path
// is acceptable. To identify a NODE's LED instead, use the Core's Nodes screen.
static void ieLedIdentify() {
#if !defined(BOARD_W550)
  pinMode(BoardPins::BACKLIGHT_PIN, OUTPUT);
  for (uint8_t i = 0; i < 3; i++) {
    digitalWrite(BoardPins::BACKLIGHT_PIN, LOW);  delay(120);
    digitalWrite(BoardPins::BACKLIGHT_PIN, HIGH); delay(120);
  }
#endif
}

static void ieHandleText(const char* text, uint16_t tlen) {
  char cmd[IE_CMD_SLOT_LEN];
  size_t n = tlen < sizeof(cmd) - 1 ? tlen : sizeof(cmd) - 1;
  memcpy(cmd, text, n); cmd[n] = '\0';

  // Trim leading/trailing whitespace + CR/LF.
  char* p = cmd;
  while (*p == ' ' || *p == '\t') p++;
  size_t plen = strlen(p);
  while (plen && (p[plen-1] == ' ' || p[plen-1] == '\r' || p[plen-1] == '\n' || p[plen-1] == '\t'))
    p[--plen] = '\0';
  if (!*p) return;

  // First word + remainder.
  char word[16];
  size_t wlen = strcspn(p, " ");
  if (wlen >= sizeof(word)) wlen = sizeof(word) - 1;
  memcpy(word, p, wlen); word[wlen] = '\0';
  const char* rest = p + strcspn(p, " ");
  while (*rest == ' ') rest++;

  if      (!strcmp(word, "help"))   ieCmdHelp();
  else if (!strcmp(word, "info"))   ieCmdInfo();
  else if (!strcmp(word, "status")) ieCmdStatus();
  else if (!strcmp(word, "caps"))   ieSendCaps();
  else if (!strcmp(word, "ch"))     { ieSetChannelPlan(rest);
                                      char pb[200]; ieFormatChannelList(pb, sizeof(pb), iePlan, iePlanCount);
                                      ieEmit("WD:PLAN chans=%s", pb); }
  else if (!strcmp(word, "led"))    { bool on = strcmp(rest, "off"); if (on) ieLedIdentify();
                                      ieEmit("LED %s", on ? "on" : "off"); }
  else if (!strcmp(word, "reboot")) { ieEmit("rebooting"); ieRebootReq = true; }
  else if (!strcmp(word, "stop"))   ieStopStreaming();
  else if (!strcmp(word, "start"))  ieStartStreaming();
  else if (!strcmp(word, "bt"))     ieEmit("mode Wi-Fi  ·  bt unsupported");
  else if (!strcmp(word, "cap"))    ieEmit("mode Wi-Fi  ·  cap unsupported");
  else if (!strcmp(word, "wdstream")) {
    if (strstr(rest, "stop")) ieStopStreaming();
    else {
      const char* chp = strstr(rest, "-ch");   // honor a channel subset the app requests
      if (chp) ieSetChannelPlan(chp + 3);
      ieStartStreaming();                       // (-wifi is implied; we always stream wifi)
    }
  }
  else { ieEmit("unknown command '%s'", word); ieCmdHelp(); }
}

// Unwrap a GB frame if present, else treat the whole write as raw text.
static void ieHandleCommand(const uint8_t* data, uint16_t len) {
  const uint8_t* text = data;
  uint16_t tlen = len;
  if (len >= FR_HEADER && data[0] == FR_MAGIC_0 && data[1] == FR_MAGIC_1) {
    uint16_t fplen = (uint16_t)(data[10] | (data[11] << 8));
    if ((size_t)FR_HEADER + fplen <= len) {
      ieReplyId = (uint32_t)data[6] | ((uint32_t)data[7] << 8) |
                  ((uint32_t)data[8] << 16) | ((uint32_t)data[9] << 24);
      if (data[3] == FR_CMD) {
        text = data + FR_HEADER;
        tlen = fplen;
        ieEnqueueFrame(FR_ACK, ieReplyId, nullptr, 0);   // ack the framed command
      }
    }
  }
  ieHandleText((const char*)text, tlen);
}

// ================================================================
//  GATT callbacks
// ================================================================
class IeCommandCB : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic* c, NimBLEConnInfo& connInfo) override {
    std::string v = c->getValue();
    if (v.empty()) return;
    uint8_t next = (ieCmdTail + 1) % IE_CMD_RING;
    if (next == ieCmdHead) return;                      // ring full -> drop
    size_t n = v.size(); if (n >= IE_CMD_SLOT_LEN) n = IE_CMD_SLOT_LEN;
    memcpy(ieCmdRing[ieCmdTail].data, v.data(), n);
    ieCmdRing[ieCmdTail].len = (uint16_t)n;
    ieCmdTail = next;
  }
};

// OTA characteristic is advertised for protocol-completeness but not wired to a
// real update path; writes are swallowed so the app never stalls on it.
class IeOtaCB : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic* c, NimBLEConnInfo& connInfo) override { (void)c; }
};

class IeTxCB : public NimBLECharacteristicCallbacks {
  void onSubscribe(NimBLECharacteristic* c, NimBLEConnInfo& connInfo, uint16_t subValue) override {
    ieNotifyOn = (subValue != 0);
    // The app auto-starts scanning when it subscribes to TX.
    if (ieNotifyOn) ieStartStreaming();
    else            ieStopStreaming();
  }
};

class IeStandaloneServerCB : public NimBLEServerCallbacks {
  void onConnect(NimBLEServer* s, NimBLEConnInfo& i) override {
    Serial.println("[IOSESP] central connected");
    // Do NOT re-arm advertising while a central is connected. Wardrive Go
    // reads an advertisement from a board it believes it is linked to as a
    // stale link ("advertising again - old link is gone"), so it terminates
    // the connection — observed as disconnect reason 531, BLE_HS_HCI_ERR(0x13)
    // "Remote User Terminated Connection" — and immediately reconnects. That
    // loops every few seconds and the stream never settles. Advertising is
    // re-armed in onDisconnect, so the board is findable again the moment the
    // phone really goes away; one central at a time is what the app expects.
    NimBLEDevice::stopAdvertising();
  }
  void onDisconnect(NimBLEServer* s, NimBLEConnInfo& i, int r) override {
    Serial.printf("[IOSESP] central disconnected (reason %d)\n", r);
    ieNotifyOn = false; ieStreaming = false;
    NimBLEDevice::startAdvertising();
  }
};

static IeCommandCB            ieCommandCb;
static IeOtaCB                ieOtaCb;
static IeTxCB                 ieTxCb;
static IeStandaloneServerCB   ieStandaloneServerCb;

// ================================================================
//  Service construction
// ================================================================
void iosEspCompatAttach(NimBLEServer* server) {
  if (!server) return;
  ieServer = server;

  NimBLEService* svc = server->createService(IE_SERVICE_UUID);
  svc->createCharacteristic(IE_CH_RX, NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR)
     ->setCallbacks(&ieCommandCb);
  chTx = svc->createCharacteristic(IE_CH_TX, NIMBLE_PROPERTY::NOTIFY);
  chTx->setCallbacks(&ieTxCb);
  svc->createCharacteristic(IE_CH_CTRL, NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR)
     ->setCallbacks(&ieCommandCb);
  svc->createCharacteristic(IE_CH_OTA, NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR)
     ->setCallbacks(&ieOtaCb);
  svc->start();

  ieNotifyOn = false;
  ieStreaming = false;
  ieTxHead = ieTxTail = 0;
  ieCmdHead = ieCmdTail = 0;
  iePlanResetToAll();          // default plan = every supported channel
  ieActive = true;
}

// Derive "iosESP-XXXX" from the last two octets of the BLE address.
static void ieBuildName() {
  std::string a = NimBLEDevice::getAddress().toString();   // "aa:bb:cc:dd:ee:ff"
  char o4 = 0, o5 = 0;  // chars of the 2nd-to-last and last octets
  if (a.length() >= 17) {
    // positions 12..13 = 2nd-to-last octet, 15..16 = last octet
    snprintf(ieName, sizeof(ieName), "iosESP-%c%c%c%c",
             toupper((unsigned char)a[12]), toupper((unsigned char)a[13]),
             toupper((unsigned char)a[15]), toupper((unsigned char)a[16]));
  } else {
    (void)o4; (void)o5;
    snprintf(ieName, sizeof(ieName), "iosESP");
  }
}

// ================================================================
//  Standalone lifecycle (Core-mode)
// ================================================================
void iosEspCompatBeginStandalone(const char* fallbackName) {
  if (ieActive) return;
  (void)fallbackName;
  if (!NimBLEDevice::isInitialized())
    NimBLEDevice::init("iosESP");
  NimBLEDevice::setMTU(247);
  NimBLEDevice::setPower(9);

  ieBuildName();
  NimBLEDevice::setDeviceName(ieName);

  NimBLEServer* server = NimBLEDevice::createServer();
  // false => NimBLE must NOT delete this static callback object on deinit.
  server->setCallbacks(&ieStandaloneServerCb, false);
  iosEspCompatAttach(server);
  ieStandalone = true;

  // Advertise the service UUID; carry the (longer) name in the scan response so
  // the 31-byte advertising payload doesn't overflow.
  NimBLEAdvertising* adv = NimBLEDevice::getAdvertising();
  NimBLEAdvertisementData advData;
  advData.setFlags(0x06);
  advData.addServiceUUID(NimBLEUUID(IE_SERVICE_UUID));
  adv->setAdvertisementData(advData);

  NimBLEAdvertisementData scanData;
  scanData.setName(ieName);
  adv->setScanResponseData(scanData);
  adv->enableScanResponse(true);

  NimBLEDevice::startAdvertising();
  Serial.printf("[IOSESP] Standalone up as \"%s\" (chip=%s)\n", ieName, IE_CHIP);
}

void iosEspCompatStopStandalone(bool freeStack) {
  if (!ieActive) return;
  ieActive = false;
  if (ieStandalone) {
    NimBLEDevice::stopAdvertising();
    if (freeStack) NimBLEDevice::deinit(true);
    ieStandalone = false;
  }
  ieNotifyOn = false;
  ieStreaming = false;
  chTx = nullptr;
  ieServer = nullptr;
  ieCmdHead = ieCmdTail = 0;
  ieTxHead = ieTxTail = 0;
  Serial.println("[IOSESP] Standalone down");
}

bool iosEspCompatActive() { return ieActive; }

void iosEspCompatDetach() {
  ieActive = false;
  ieStandalone = false;
  ieNotifyOn = false;
  ieStreaming = false;
  chTx = nullptr;
  ieServer = nullptr;
  ieCmdHead = ieCmdTail = 0;
  ieTxHead = ieTxTail = 0;
}

// ================================================================
//  Telemetry push
// ================================================================
static char lowerHex(uint8_t v) { return (char)(v < 10 ? '0' + v : 'a' + (v - 10)); }

void iosEspCompatPushAp(const char* bssid, const char* ssid,
                        int channel, int rssi, const char* auth) {
  if (!ieActive || !ieNotifyOn || !ieStreaming) return;
  if (iePlanCount && !ieChannelInPlan(channel)) return;   // app narrowed the plan
  if (rssi < -128) rssi = -128; else if (rssi > 0) rssi = rssi;   // dBm, negative

  // SSID -> lower-case hex; empty => hidden.
  char ssidHex[2 * 33 + 1];
  size_t hi = 0;
  if (ssid) {
    for (size_t i = 0; ssid[i] && hi < sizeof(ssidHex) - 2; i++) {
      ssidHex[hi++] = lowerHex((uint8_t)ssid[i] >> 4);
      ssidHex[hi++] = lowerHex((uint8_t)ssid[i] & 0x0F);
    }
  }
  ssidHex[hi] = '\0';
  unsigned hidden = (hi == 0) ? 1u : 0u;

  ieEmit("WD:AP ts=%lu bssid=%s ssid_hex=%s rssi=%d ch=%d auth=%s hidden=%u",
         (unsigned long)millis(), bssid ? bssid : "00:00:00:00:00:00",
         ssidHex, rssi, channel, ieAuthToken(auth), hidden);
  ieApSweep++;
}

void iosEspCompatPushGps(double lat, double lon, double altMeters, int sats) {
  if (!ieActive || !ieNotifyOn) return;
  iosEspCompatSetGpsPresent(true);           // a fix proves a module is attached
  if (!ieStreaming) return;
  if (sats < 0) sats = 0;
  ieEmit("WD:GPS ts=%lu lat=%.6f lon=%.6f alt=%.1f fix=1 sats=%d",
         (unsigned long)millis(), lat, lon, altMeters, sats);
}

// ================================================================
//  Main-loop tick
// ================================================================
void iosEspCompatTick() {
  if (!ieActive) return;

  // 1. Handle inbound commands (replies/handshake are enqueued as frames).
  while (ieCmdHead != ieCmdTail) {
    uint8_t i = ieCmdHead;
    ieCmdHead = (ieCmdHead + 1) % IE_CMD_RING;
    ieHandleCommand(ieCmdRing[i].data, ieCmdRing[i].len);
  }

  // 2. Drain outbound frames to the phone, WITH BACKPRESSURE. NimBLE's notify()
  // returns false when the controller is out of buffers, and any notifications
  // past that point are silently dropped — not queued. A back-to-back burst
  // (the 5-line scan-start handshake: ACK, WD:CAPS, WD:MODE, WD:BEGIN, WD:PLAN)
  // overruns the buffers, so its first frames were being lost and the app never
  // got its ACK/WD:CAPS and hung on "Starting stream...". So we advance the ring
  // only when the notify is accepted, and leave the rest for the next tick.
  if (ieNotifyOn && chTx) {
    uint16_t sent = 0;
    while (ieTxHead != ieTxTail && sent < IE_TX_PER_TICK) {
      uint8_t i = ieTxHead;
      if (!chTx->notify(ieTxRing[i].data, ieTxRing[i].len)) break;  // buffers full: retry next tick
      ieTxHead = (ieTxHead + 1) % IE_TX_RING;
      sent++;
    }
    ieSent += sent;
  }

  // 3. Periodic WD:STATUS heartbeat (one "sweep" ~ every 5 s).
  if (ieStreaming && ieNotifyOn) {
    uint32_t now = millis();
    if (now - ieLastStat >= 5000) {
      ieLastStat = now;
      ieSweeps++;
      ieEmit("WD:STATUS aps=%lu scans=%lu", (unsigned long)ieApSweep, (unsigned long)ieSweeps);
      ieApSweep = 0;

      static uint32_t dbgMs = 0;
      if (now - dbgMs >= 3000) {
        dbgMs = now;
        uint8_t depth = (uint8_t)((ieTxTail - ieTxHead + IE_TX_RING) % IE_TX_RING);
        Serial.printf("[IOSESP] tx enq=%lu sent=%lu dropFull=%lu depth=%u\n",
                      (unsigned long)ieEnq, (unsigned long)ieSent,
                      (unsigned long)ieDropFull, depth);
      }
    }
  }

  // 4. Honor an app-requested reboot once the reply has had a chance to flush.
  if (ieRebootReq && ieTxHead == ieTxTail) {
    delay(60);
    ESP.restart();
  }
}
