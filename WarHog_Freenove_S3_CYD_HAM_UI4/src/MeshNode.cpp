#include "MeshNode.h"
#include "PsramAlloc.h"   // big rings live in PSRAM (internal DRAM is scarce)
#include "Globals.h"
#include "Config.h"
#include "BiscuitCompat.h"   // present as a Biscuit Pro to the Biscuit Manager app while in Core mode
#include "IosEspCompat.h"    // or present as a RocketGod iosESP bridge to the Wardrive Go app (toggle in settings)
#include "GPS.h"
#include "SDUtils.h"
#include "BleScanner.h"
// The solo phone bridge shares the NimBLE stack with these, so it needs their
// "is active" predicates to decide whether its teardown may free the controller.
#include "BleDetectors.h"
#include "AppleBleSpam.h"
#include "BleSpam.h"
#include "ScreenTelemetry.h"
#include "pig_dedup.h"   // Scheduler Phase 1: shared worker dedup (flag PIG_FEAT_DEDUP, default OFF)
#include "pig_dwell.h"   // Scheduler Phase 2: adaptive dwell + DFS passive (flag PIG_FEAT_ADAPTIVE_DWELL, default OFF)
#include "pig_proto.h"   // Scheduler Phase 3: PIG_MSG_ADMIN_EXT explicit channel list (worker parse)
#include "SpiBusLock.h"  // Scheduler Phase 4: recursive mutex serializing cross-core SD access
#include "esp_heap_caps.h"
#include "mbedtls/sha256.h"   // OTA: whole-image SHA-256 sent to the node in OTA_BEGIN

// ================================================================
//  Scheduler Phase 4 (Step 1) — move Core-mode per-record SD logging off the
//  main loop onto a FreeRTOS task pinned to CORE 0 (the Arduino loop runs on
//  core 1), so the RT path (ESP-NOW RX ring drain + reassignment in
//  coreModeTick) keeps running on core 1 while the SD write proceeds in
//  parallel on core 0. Unlike the single-core C5, the S3 is dual-core, so this
//  is a real parallel offload. The task takes gSpiBusMutex (SpiBusLock.h)
//  around the write, and every core-1 SD user (file moves, config/profile
//  saves) takes the same mutex, so the two cores never interleave SD access. Flag-gated
//  PIG_FEAT_CORE_ISO (default OFF -> inline write, no task, no mutex,
//  byte-identical). Lossy queue: drops newest on overflow rather than blocking.
// ================================================================
#ifndef PIG_FEAT_CORE_ISO
#define PIG_FEAT_CORE_ISO 0
#endif
#if PIG_FEAT_CORE_ISO
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
SemaphoreHandle_t gSpiBusMutex = nullptr;   // cross-core SD access lock (SpiBusLock.h)
struct PigLogRec {
  char   mac[20], ssid[34], auth[20], ts[28];
  int16_t ch; int8_t rssi; double lat, lon, alt, acc;
};
static QueueHandle_t gLogQ      = nullptr;
static TaskHandle_t  gStoreTask = nullptr;
static volatile bool gStoreRun  = false;
static volatile uint32_t gStoreDrops = 0;   // records dropped when the SD queue is full
#if PIG_FEAT_CORE_ISO
// Phase 4 acceptance instrumentation (DESIGN §13: "0 RX deadline misses; RX jitter
// p95 < 5 ms @ ≥12 nodes"). An ESP-NOW packet dropped because its RX ring was full
// IS a deadline miss — the core-1 drain couldn't keep up. The coreModeTick period is
// the drain cadence; a tick gap > 5 ms is a jitter violation. rx/req drops are
// cumulative since boot; loop max/gt5 reset each 5 s line so each window stands alone.
static volatile uint32_t gRxTextDrops = 0;  // JCMK_MSG_TEXT dropped: coreTextBuf full
static volatile uint32_t gRxReqDrops  = 0;  // CORE_REQUEST dropped: coreReqBuf full
static uint32_t gLoopLastUs = 0;            // previous coreModeTick micros()
static uint32_t gLoopMaxUs  = 0;            // widest tick gap this window
static uint32_t gLoopGt5    = 0;            // ticks exceeding 5 ms this window

// Phase 4 Step 5: the ESP-NOW TEXT-record drain moves off the main loop onto a
// dedicated core-0 task, so record servicing no longer inherits the loop's 95–218 ms
// jitter (measured). The task does the RT/SD half (parse → gChanHits → SD enqueue);
// the non-RT half (phone-app mirror + UI telemetry) is handed back to the loop via
// coreMirrorBuf and runs on core 1 — the same core as the BLE bridge tick — so NimBLE
// and UI state are only ever touched from one core. SPSC rings: coreTextBuf producer is
// the RX callback, consumer is this task; coreMirrorBuf producer is this task, consumer
// is the loop. rt_* track the task's own cadence = the real RX-servicing jitter.
struct CoreMirrorRec { char bssid[18], ssid[34], auth[12]; int16_t ch, rssi; };
static CoreMirrorRec*    coreMirrorBuf = psramCalloc<CoreMirrorRec>(64);
static volatile uint8_t  coreMirrorHead = 0, coreMirrorTail = 0;
static TaskHandle_t      gRxDrainTask = nullptr;
static volatile bool     gRxDrainRun  = false;
static volatile uint16_t gRxQMax      = 0;  // max coreTextBuf occupancy the drain saw
static uint32_t          gRtLastUs = 0, gRtMaxUs = 0, gRtGt5 = 0;  // drain-task cadence
#endif
static void tStore(void*) {
  PigLogRec r;
  while (gStoreRun) {
    if (xQueueReceive(gLogQ, &r, pdMS_TO_TICKS(20)) == pdTRUE) {
      spiBusLock();   // core-1 SD users take the same lock
      appendWigleRow(String(r.mac), String(r.ssid), String(r.auth), String(r.ts),
                     r.ch, r.rssi, r.lat, r.lon, r.alt, r.acc);
      spiBusUnlock();
    }
  }
  vTaskDelete(nullptr);
}
static void coreStoreStart() {
  if (gStoreTask) return;
  if (!gSpiBusMutex) gSpiBusMutex = xSemaphoreCreateRecursiveMutex();  // before the task can write
  if (!gLogQ) gLogQ = xQueueCreate(48, sizeof(PigLogRec));
  gStoreRun = true;
  xTaskCreatePinnedToCore(tStore, "tStore", 6144, nullptr, 2, &gStoreTask, 0);  // core 0, prio 2 (below WiFi)
  Serial.println("[CORE-ISO] SD store task started (core 0)");
}
static void coreStoreStop() {
  if (!gStoreTask) return;
  gStoreRun = false;   // task self-deletes on its next wake; mutex kept for present()
  gStoreTask = nullptr;
}
// Returns true if queued (false = dropped under backpressure).
static bool coreStoreEnqueue(const String& mac, const String& ssid, const String& auth,
                             const String& ts, int ch, int rssi,
                             double lat, double lon, double alt, double acc) {
  if (!gLogQ) return false;
  PigLogRec r{};
  snprintf(r.mac,  sizeof(r.mac),  "%s", mac.c_str());
  snprintf(r.ssid, sizeof(r.ssid), "%s", ssid.c_str());
  snprintf(r.auth, sizeof(r.auth), "%s", auth.c_str());
  snprintf(r.ts,   sizeof(r.ts),   "%s", ts.c_str());
  r.ch = (int16_t)ch; r.rssi = (int8_t)rssi; r.lat = lat; r.lon = lon; r.alt = alt; r.acc = acc;
  bool ok = (xQueueSend(gLogQ, &r, 0) == pdTRUE);   // 0 timeout -> drop if full, never block
  if (!ok) gStoreDrops++;
  return ok;
}
#endif

// ================================================================
//  Scheduler Phase 3 — dynamic yield-weighted channel assignment (flag-gated).
//  When PIG_FEAT_DYNAMIC_ASSIGN=1 the Core tracks per-channel yield from the
//  records it receives and hands dual-band nodes their 5 GHz channels in YIELD
//  order (productive channels first) instead of a flat contiguous split. Stays
//  band-aware: 2.4 GHz-only (B24) nodes keep {1,6,11} untouched. OFF by default
//  -> the legacy contiguous split, zero behavior change.
// ================================================================
#ifndef PIG_FEAT_DYNAMIC_ASSIGN
#define PIG_FEAT_DYNAMIC_ASSIGN 0
#endif
#if PIG_FEAT_DYNAMIC_ASSIGN
#include "yield_sched.h"   // scheduler/ (via -I../scheduler)
static ChanStat  gChanStat[PIG_MAX_CH];   // per-channel yield EWMA (idx matches JCMK_CHANNELS)
static uint16_t  gChanHits[PIG_MAX_CH];    // records seen per channel this window
static uint32_t  gYieldLastMs = 0;
static SchedCfg  gSchedCfg;
static int chIndexOf(uint8_t ch) {
  for (uint8_t i = 0; i < JCMK_NUM_CHANNELS; i++) if (JCMK_CHANNELS[i] == ch) return (int)i;
  return -1;
}
// Fold accumulated per-channel record counts into the yield EWMA once per window.
static void coreYieldUpdate(uint32_t now) {
  if (gYieldLastMs == 0) { gYieldLastMs = now; return; }
  if (now - gYieldLastMs < 4000) return;              // 4 s window
  float dt = (float)(now - gYieldLastMs) / 1000.0f;
  for (uint8_t i = 0; i < JCMK_NUM_CHANNELS; i++) {
    uint8_t h = gChanHits[i] > 255 ? 255 : (uint8_t)gChanHits[i];
    sched_on_yield(gChanStat, i, h, h, dt, gSchedCfg);
    gChanHits[i] = 0;
  }
  gYieldLastMs = now;
}
#endif
#include <esp_now.h>
#include "esp_wifi.h"

// ================================================================
//  Protocol constants
// ================================================================
const uint8_t  JCMK_ESPNOW_CH       = 6;
static const uint8_t  JCMK_MAGIC[4] = {'E','N','O','W'};
static const uint8_t  JCMK_BCAST[6] = {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF};
static const uint32_t JCMK_REQ_INIT_MS = 300;
static const uint32_t JCMK_REQ_MAX_MS  = 5000;
static const uint32_t JCMK_HB_MS          = 5000;
static const uint32_t NODE_SCAN_DWELL_MS  = 80;    // ms per channel (JCMK CHANNEL_TIMER)
static const uint32_t NODE_ADMIN_WIN_MS   = 500;   // ch-6 window after each full cycle
#define JCMK_TEXT_MAX 200

// Which phone-app BLE personality this Core-mode session advertises. Latched at
// enterCoreMode() from cfg.blePersonality so begin/stop/tick/push stay symmetric
// even if the setting is toggled mid-session (takes effect on next Core entry).
static bool coreBleIsIosEsp = false;

// WarHog mesh-size extension: the Core stamps the total active-node count behind
// this marker in its broadcast heartbeat so every node can show the true herd
// count. Stock JCMK/Biscuit nodes ignore heartbeat text, so this is harmless.
// Must match the marker in warhog_fork and c3_node.
static const char*   WARHOG_MARKER     = "WARHOG1";
static const uint8_t WARHOG_MARKER_LEN = 7;

enum JcmkMsgType : uint8_t {
  JCMK_MSG_CORE_REQUEST = 1,
  JCMK_MSG_CORE_REPLY   = 2,
  JCMK_MSG_HEARTBEAT    = 3,
  JCMK_MSG_TEXT         = 4,
  JCMK_MSG_ADMIN        = 5
};

// Packed structs match JCMK wire layout exactly
typedef struct __attribute__((packed)) {
  char     magic[4];
  uint8_t  type;
  uint32_t counter;
  uint16_t len;
  char     text[JCMK_TEXT_MAX + 1];
} jcmk_text_msg_t;

typedef struct __attribute__((packed)) {
  char    magic[4];
  uint8_t type;
  uint8_t assignment_version;
  uint8_t node_index;
  uint8_t node_count;
  uint8_t start_channel_idx;
  uint8_t end_channel_idx;
} jcmk_admin_msg_t;

typedef struct __attribute__((packed)) {
  char     magic[4];
  uint8_t  type;
  uint32_t counter;
} jcmk_hb_msg_t;

typedef struct __attribute__((packed)) {
  char    magic[4];
  uint8_t type;
} jcmk_req_msg_t;

// JCMK scan-channel table — must match Core exactly
const uint8_t JCMK_CHANNELS[] = {
  1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14,
  36, 40, 44, 48, 52, 56, 60, 64,
  100, 112, 116, 120, 124, 128, 132, 136, 140, 144,
  149, 153, 157, 161, 165, 169, 173, 177
};
const uint8_t JCMK_NUM_CHANNELS = (uint8_t)(sizeof(JCMK_CHANNELS));

// ================================================================
//  Node state
// ================================================================
bool     meshNodeActive   = false;
bool     jcmkHaveCore     = false;
uint8_t  jcmkCoreMac[6]  = {0};
uint8_t  jcmkStartIdx    = 0;
uint8_t  jcmkEndIdx      = 0;
uint8_t  jcmkAssignVer   = 0;
uint32_t jcmkNetworksFound = 0;
uint32_t jcmkSentCount   = 0;

static uint32_t jcmkHbCounter   = 0;
static uint32_t jcmkLastHbMs    = 0;
static uint32_t jcmkLastReqMs   = 0;
static uint32_t jcmkReqInterval = JCMK_REQ_INIT_MS;

// Per-channel async scan state
static bool     nodeScanActive   = false;
static bool     nodeScanAdminWin = false;
static uint32_t nodeScanAdminMs  = 0;
// Phase 2 two-pass sweep (scheduler/pig_dwell.h) — it now owns the channel
// cursor the scan tick used to keep by hand. Zero-initialized means "needs
// rebuild", so the first scan tick builds the list from the current assignment
// and every site that restarts the cycle just invalidates it.
static PigSweep nodeSweep;

// Pending core-found event — written in ESP-Now callback, consumed in loop
static volatile bool  jcmkCoreFoundPending = false;
static uint8_t        jcmkCoreMacPending[6] = {0};

// Biscuit plain-reply (encrypt=0) interop. Set when a received CORE_REPLY
// advertises "encrypt=0"; gates the type=5/type=10 config-handshake handling
// and the bracketed-auth scan-result format a plain-reply Biscuit Core
// expects. Never set when paired with a WarHog/JCMK Core, so the existing
// Core<->node path is unchanged.
static volatile bool  biscuitPlainReplyPending = false;
static uint8_t        biscuitChannels[JCMK_NUM_CHANNELS + 2];
static volatile uint8_t biscuitChannelCount = 0;

// ================================================================
//  Core mode state
// ================================================================
bool         meshCoreActive = false;
uint32_t     coreRecordsRx  = 0;
uint8_t      coreNodeCount  = 0;
CoreNodeInfo coreNodes[CORE_MAX_NODES] = {};

static uint8_t  coreAssignVer  = 0;
static uint32_t coreLastHbMs   = 0;
static uint32_t coreHbCounter  = 0;

// Sub-cluster membership remembered by MAC so a node that times out and
// rejoins returns to the group the user put it in (coreNodes[] itself is
// cleared on timeout). Default group is 1 for any MAC not listed here.
struct GroupMem { uint8_t mac[6]; uint8_t group; };
static GroupMem groupMem[CORE_MAX_NODES] = {};
static uint8_t  groupMemCount = 0;

static uint8_t rememberedGroup(const uint8_t* mac) {
  for (uint8_t i = 0; i < groupMemCount; i++)
    if (memcmp(groupMem[i].mac, mac, 6) == 0) return groupMem[i].group;
  return 1;  // default sub-cluster
}

static void rememberGroup(const uint8_t* mac, uint8_t group) {
  for (uint8_t i = 0; i < groupMemCount; i++) {
    if (memcmp(groupMem[i].mac, mac, 6) == 0) { groupMem[i].group = group; return; }
  }
  if (groupMemCount < CORE_MAX_NODES) {
    memcpy(groupMem[groupMemCount].mac, mac, 6);
    groupMem[groupMemCount].group = group;
    groupMemCount++;
  }
}

static const uint32_t CORE_HB_MS        = 5000;
static const uint32_t CORE_NODE_TIMEOUT = 90000;  // 90 s — generous for many-node ESP-Now collisions

// Ring buffers: ESP-Now callback → main loop
#define CORE_REQ_QUEUE  16
#define CORE_TEXT_QUEUE 192  // sized for burst from 12 nodes × ~15 networks each

struct CorReqSlot  { uint8_t mac[6]; bool isBiscuit; bool band24; };

// Dwell (ms) the Core tells nodes to spend per channel. Longer than the old 80 ms
// so a node reliably catches a beacon (APs beacon ~every 100 ms) now that
// band-aware assignment gives each node fewer, better channels.
static const uint32_t MESH_NODE_DWELL_MS = 110;

// A node declares itself 2.4 GHz-only by putting the marker "B24" in its
// CORE_REQUEST / HEARTBEAT text; the Core then never assigns it 5 GHz.
static bool jcmkPacketIsBand24(const uint8_t* data, int len) {
  if (len < (int)sizeof(jcmk_text_msg_t)) return false;
  const jcmk_text_msg_t* tm = (const jcmk_text_msg_t*)data;
  uint16_t slen = (tm->len < JCMK_TEXT_MAX) ? tm->len : JCMK_TEXT_MAX;
  for (uint16_t i = 0; i + 3 <= slen; i++)
    if (tm->text[i] == 'B' && tm->text[i + 1] == '2' && tm->text[i + 2] == '4') return true;
  return false;
}
struct CorTextSlot {
  char    line[JCMK_TEXT_MAX + 1];
#if PIG_FEAT_PAIR_SCHED
  uint8_t srcNode;   // index into coreNodes[] of the reporting node (0xFF = unknown)
#endif
};

static CorReqSlot          coreReqBuf[CORE_REQ_QUEUE];
static volatile uint8_t    coreReqHead = 0, coreReqTail = 0;

static CorTextSlot*        coreTextBuf = psramCalloc<CorTextSlot>(CORE_TEXT_QUEUE);
static volatile uint8_t    coreTextHead = 0, coreTextTail = 0;

// ================================================================
//  Local helpers
// ================================================================
static String meshAuthModeToString(wifi_auth_mode_t m) {
  switch (m) {
    case WIFI_AUTH_OPEN:            return "OPEN";
    case WIFI_AUTH_WEP:             return "WEP";
    case WIFI_AUTH_WPA_PSK:         return "WPA";
    case WIFI_AUTH_WPA2_PSK:        return "WPA2";
    case WIFI_AUTH_WPA_WPA2_PSK:    return "WPAWPA2";
    case WIFI_AUTH_WPA2_ENTERPRISE: return "WPA2EAP";
    case WIFI_AUTH_WPA3_PSK:        return "WPA3";
    case WIFI_AUTH_WPA2_WPA3_PSK:   return "WPA2WPA3";
    default: return "UNKNOWN";
  }
}

// ================================================================
//  ESP-Now helpers
// ================================================================
static void jcmkSetChannel(uint8_t ch) {
  esp_wifi_set_ps(WIFI_PS_NONE);
  // Do NOT use promiscuous mode here: on IDF 5.x, disabling promiscuous after
  // a prior STA connection causes the driver to revert to the home router channel.
  // Direct esp_wifi_set_channel() works correctly when the STA is not connected.
  esp_wifi_set_channel(ch, WIFI_SECOND_CHAN_NONE);
}

static bool jcmkAddPeer(const uint8_t* mac) {
  if (esp_now_is_peer_exist(mac)) esp_now_del_peer(mac);
  esp_now_peer_info_t peer = {};
  memcpy(peer.peer_addr, mac, 6);
  peer.channel = 0;  // follow current home channel
  peer.encrypt = false;
  return (esp_now_add_peer(&peer) == ESP_OK);
}

// Node-mode sends always use full jcmk_text_msg_t (212 bytes).
// Biscuit Pro drops any packet < 212 bytes; JCMK Cores only check len >= 5
// so full-size packets are backward-compatible with both coordinators.

// A non-C5 board is 2.4 GHz-only: declare "B24" so the Core never assigns it
// 5 GHz channels it can't tune. C5 boards send nothing (default = dual-band).
static void jcmkStampBand(jcmk_text_msg_t& msg) {
  if (!wardriverIsC5()) { memcpy(msg.text, "B24", 3); msg.text[3] = '\0'; msg.len = 3; }
  else                  { msg.len = 0; }
}

static void jcmkSendCoreRequest() {
  jcmk_text_msg_t msg = {};
  memcpy(msg.magic, JCMK_MAGIC, 4);
  msg.type    = JCMK_MSG_CORE_REQUEST;
  msg.counter = 0;
  jcmkStampBand(msg);
  esp_now_send(JCMK_BCAST, (uint8_t*)&msg, sizeof(msg));
}

static void jcmkSendHeartbeat() {
  jcmk_text_msg_t msg = {};
  memcpy(msg.magic, JCMK_MAGIC, 4);
  msg.type    = JCMK_MSG_HEARTBEAT;
  msg.counter = ++jcmkHbCounter;
  jcmkStampBand(msg);
  esp_now_send(JCMK_BCAST, (uint8_t*)&msg, sizeof(msg));
}

static void jcmkSendText(const String& s) {
  jcmk_text_msg_t msg = {};
  memcpy(msg.magic, JCMK_MAGIC, 4);
  msg.type    = JCMK_MSG_TEXT;
  msg.counter = jcmkHbCounter;
  uint16_t slen = (uint16_t)((s.length() < JCMK_TEXT_MAX) ? s.length() : JCMK_TEXT_MAX);
  msg.len = slen;
  memcpy(msg.text, s.c_str(), slen);
  msg.text[slen] = '\0';
  // Always send full struct size — Biscuit Pro drops variable-length packets < 212 bytes.
  esp_now_send(JCMK_BCAST, (uint8_t*)&msg, sizeof(msg));
}

// ================================================================
//  Biscuit Reporter state (solo mode)
// ================================================================
bool     biscuitReporterActive = false;
bool     biscuitLinked         = false;
uint32_t biscuitRecordsSent    = 0;

static bool     biscuitInited     = false;
static uint32_t biscuitLastHbMs   = 0;
static uint32_t biscuitLastReqMs  = 0;

#define BISCUIT_FWD_QUEUE 96
static char (*biscuitFwd)[JCMK_TEXT_MAX + 1] = psramCalloc<char[JCMK_TEXT_MAX + 1]>(BISCUIT_FWD_QUEUE);
static int  biscuitFwdCount = 0;

// ================================================================
//  Core mode helpers (forward declarations used in jcmkOnRecv)
// ================================================================
static void coreSendReply(const uint8_t* mac) {
  // Use full-size jcmk_text_msg_t (212 bytes) instead of jcmk_req_msg_t (5 bytes).
  // Biscuit Node drops any packet < sizeof(enow_text_msg_t) = 212 bytes.
  // JCMK nodes only check len >= 5, so this is fully backward-compatible.
  jcmk_text_msg_t msg = {};
  memcpy(msg.magic, JCMK_MAGIC, 4);
  msg.type    = JCMK_MSG_CORE_REPLY;
  msg.counter = 0;
  msg.len     = 0;
  esp_now_send(mac, (uint8_t*)&msg, sizeof(msg));
}

// Tell one node to blink its LED (PIG_MSG_IDENTIFY) so a human can tell which
// physical board that roster entry is. Unicast to the node, which is already a
// registered ESP-NOW peer. WarHog-only: legacy JCMK/Biscuit nodes ignore the
// unknown type, so it is harmless to send to a mixed mesh. A tiny (6-byte)
// control frame — unlike record packets it needn't be padded to 212 B.
static void coreSendIdentify(const uint8_t* mac, uint8_t seconds) {
  pig_identify_t msg = {};
  memcpy(msg.magic, JCMK_MAGIC, 4);
  msg.type    = PIG_MSG_IDENTIFY;
  msg.seconds = seconds;
  esp_now_send(mac, (uint8_t*)&msg, sizeof(msg));
}

// Public: blink the LED of the node at coreNodes[nodeIdx]. Called from the
// on-device Nodes UI. Sent a few times because a single broadcast-channel frame
// is easily lost while the node is off scanning another channel.
void coreIdentifyNode(uint8_t nodeIdx) {
  if (!meshCoreActive || nodeIdx >= CORE_MAX_NODES || !coreNodes[nodeIdx].active) return;
  for (uint8_t r = 0; r < 3; r++) {
    coreSendIdentify(coreNodes[nodeIdx].mac, 6);
    delay(10);
  }
}


// ================================================================
//  OTA update of a worker node (PIG_MSG_OTA_BEGIN / _DATA / _ACK)
// ================================================================
// The update image is read off the SD card ONCE, at boot (coreOtaPreload), into
// PSRAM and streamed from there. Reading the SD card later, while Core mode is
// running, is not reliable on this board: the Core runs with only ~10 KB of free
// internal RAM (BLE bridge + Wi-Fi) and SD_MMC reads then silently return stale
// data. So to push a new image: put it on the SD card and reboot the Core.
//
// State machine driven from coreModeTick() via coreOtaTick():
//   Starting — resend OTA_BEGIN until the node answers READY
//   Sending  — go-back-N: keep PIG_OTA_WINDOW chunks in flight, advance on the
//              node's cumulative acks, rewind to the last ack on a timeout
//   Done / Failed — result shown on the node-detail screen for a while
// ACKs arrive in the ESP-NOW callback and are only recorded there (volatile
// flags); all sends happen from the loop.
static const uint32_t OTA_BEGIN_RETRY_MS   = 400;     // resend OTA_BEGIN this often
static const uint32_t OTA_START_TIMEOUT_MS = 20000;   // node never answered
static const uint32_t OTA_ACK_TIMEOUT_MS   = 250;     // no ack progress -> resend window
static const uint32_t OTA_STALL_MS         = 10000;   // no ack progress at all -> give up
static const uint32_t OTA_RESULT_SHOW_MS   = 15000;   // keep Done/Failed visible this long
static const uint32_t OTA_MAX_IMAGE        = 0x330000; // size of an ota slot in default_8MB.csv
static const uint16_t OTA_CHIP_ESP32C5     = 23;      // esp_image_header_t.chip_id for the C5
static const uint16_t OTA_CHIP_ESP32C3     = 5;       // esp_image_header_t.chip_id for the C3

// Preloaded node images, one per node chip. Firmware is chip-specific, so the
// Core streams each node the image matching it: a dual-band roster entry is a C5
// node, a 2.4 GHz-only one (band24Only) is the C3 OLED node. Each file is the
// app-only *.ota.bin CI publishes for that board; put the ones you use in /fw/
// on the Core's SD card. Missing files are simply unavailable (not an error until
// a node of that chip is selected for update).
struct CoreOtaImage {
  uint16_t    chip;         // esp_image_header chip_id this file must carry
  const char* path;         // /fw/<env>.ota.bin on the SD card
  const char* label;        // short tag for status messages
  uint8_t*    image;        // whole image in PSRAM, null if not loaded
  uint32_t    size;
  uint16_t    chunks;
  uint8_t     sha[32];
  char        failMsg[28];  // why this image is not loaded
};
static CoreOtaImage otaImages[] = {
  { OTA_CHIP_ESP32C5, "/fw/warhognode_c5.ota.bin", "C5", nullptr, 0, 0, {0}, {0} },
  { OTA_CHIP_ESP32C3, "/fw/esp32c3_oled.ota.bin",  "C3", nullptr, 0, 0, {0}, {0} },
};
static const int OTA_IMAGE_COUNT = (int)(sizeof(otaImages) / sizeof(otaImages[0]));

static struct {
  CoreOtaPhase phase   = CoreOtaPhase::Idle;
  int8_t       node    = -1;
  uint8_t      mac[6]  = {0};
  uint8_t      session = 0;
  uint8_t*     image   = nullptr;   // whole node image, in PSRAM (null = none loaded)
  uint32_t     size    = 0;
  uint16_t     chunks  = 0;
  uint8_t      sha[32] = {0};
  char         loadMsg[28] = {0};   // why no image is loaded
  uint16_t     base    = 0;         // chunks the node has acknowledged
  uint16_t     nextSend = 0;
  uint32_t     startMs = 0, lastBeginMs = 0, ackMs = 0, progressMs = 0, endMs = 0;
  char         msg[28] = {0};
} ota;

// Written by the ESP-NOW callback, consumed by coreOtaTick().
static volatile bool     otaGotReady  = false;
static volatile uint16_t otaAckNext   = 0;
static volatile uint8_t  otaFinalStat = 0xFF;   // PIG_OTA_ST_DONE / PIG_OTA_ST_ERR_*; 0xFF = none yet

static bool coreOtaBusy() {
  return ota.phase == CoreOtaPhase::Starting || ota.phase == CoreOtaPhase::Sending;
}

static void coreOtaSetMsg(const char* m) {
  strlcpy(ota.msg, m, sizeof(ota.msg));
  if (m[0]) Serial.printf("[OTA] %s\n", m);
}

static void coreOtaLoadFail(const char* m) {
  strlcpy(ota.loadMsg, m, sizeof(ota.loadMsg));
  Serial.printf("[OTA] no image loaded: %s\n", m);
}

// Load one node image file from SD into PSRAM, validating its chip. Records the
// reason in im.failMsg if it is unavailable. A missing file is not an error here
// (the chip may simply not be in this mesh) — it only matters at coreOtaStart().
static void coreOtaLoadOne(CoreOtaImage& im) {
  if (im.image) { heap_caps_free(im.image); im.image = nullptr; }
  im.size = 0; im.chunks = 0; im.failMsg[0] = 0;
  if (!SD.exists(im.path)) { strlcpy(im.failMsg, "NO IMAGE ON SD", sizeof(im.failMsg)); return; }
  SdReader f(im.path);                 // DMA-safe read (see SDUtils.h)
  if (!f) { strlcpy(im.failMsg, "CANNOT OPEN IMAGE", sizeof(im.failMsg)); return; }
  uint32_t size = (uint32_t)f.size();
  if (size < 1024 || size > OTA_MAX_IMAGE) { f.close(); strlcpy(im.failMsg, "BAD IMAGE SIZE", sizeof(im.failMsg)); return; }

  uint8_t* img = (uint8_t*)heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  const size_t IO = 4096;   // SD_MMC DMA-reads into an internal buffer; copy out to PSRAM
  uint8_t* io = (uint8_t*)heap_caps_malloc(IO, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA | MALLOC_CAP_8BIT);
  if (!img || !io) {
    if (img) heap_caps_free(img);
    if (io)  heap_caps_free(io);
    f.close();
    strlcpy(im.failMsg, "OUT OF MEMORY", sizeof(im.failMsg));
    return;
  }
  uint32_t got = 0;
  while (got < size) {
    uint32_t want = size - got;
    if (want > IO) want = IO;
    int n = f.read(io, want);
    if (n <= 0) break;
    memcpy(img + got, io, (size_t)n);
    got += (uint32_t)n;
  }
  f.close();
  heap_caps_free(io);
  if (got != size) { heap_caps_free(img); strlcpy(im.failMsg, "SD READ ERROR", sizeof(im.failMsg)); return; }

  uint16_t chip = (uint16_t)(img[12] | (img[13] << 8));
  if (img[0] != 0xE9)      { heap_caps_free(img); strlcpy(im.failMsg, "NOT AN APP IMAGE", sizeof(im.failMsg)); return; }
  if (chip != im.chip)     { heap_caps_free(img); snprintf(im.failMsg, sizeof(im.failMsg), "NOT A %s IMAGE", im.label); return; }

  mbedtls_sha256(img, size, im.sha, 0);
  im.image  = img;
  im.size   = size;
  im.chunks = (uint16_t)((size + PIG_OTA_CHUNK - 1) / PIG_OTA_CHUNK);
  Serial.printf("[OTA] %s node image loaded: %lu bytes, %u chunks\n", im.label, (unsigned long)size, (unsigned)im.chunks);
}

// Call once from setup(), right after the SD card is mounted and BEFORE the
// radios / BLE start (see the note above). Loads every node image present on SD
// and leaves a short per-chip summary in ota.loadMsg for the UI.
void coreOtaPreload() {
  ota.loadMsg[0] = 0;
  if (!sdOk) { coreOtaLoadFail("NO SD CARD"); return; }
  int loaded = 0;
  char summary[28] = {0};
  for (int i = 0; i < OTA_IMAGE_COUNT; i++) {
    coreOtaLoadOne(otaImages[i]);
    if (otaImages[i].image) loaded++;
    // "C5:ok C3:-" style, truncated to the loadMsg width.
    char part[12];
    snprintf(part, sizeof(part), "%s%s:%s", summary[0] ? " " : "", otaImages[i].label,
             otaImages[i].image ? "ok" : "-");
    strlcat(summary, part, sizeof(summary));
  }
  if (loaded == 0) coreOtaLoadFail("NO NODE IMAGE ON SD");
  else             strlcpy(ota.loadMsg, summary, sizeof(ota.loadMsg));
}

static void coreOtaClose(CoreOtaPhase endPhase, const char* msg) {
  ota.phase = endPhase;
  ota.endMs = millis();
  coreOtaSetMsg(msg);
}

// ESP-NOW callback context: record only, no sends.
static void coreOtaOnAck(const uint8_t* mac, const uint8_t* data, int len) {
  if (len < (int)sizeof(pig_ota_ack_t)) return;
  if (ota.phase != CoreOtaPhase::Starting && ota.phase != CoreOtaPhase::Sending) return;
  if (memcmp(mac, ota.mac, 6) != 0) return;
  const pig_ota_ack_t* a = (const pig_ota_ack_t*)data;
  if (a->session != ota.session) return;
  if (ota.node >= 0) coreNodes[ota.node].lastHbMs = millis();   // an ack proves the node is alive
  if (a->status == PIG_OTA_ST_READY) otaGotReady = true;
  else if (a->status >= PIG_OTA_ST_DONE) otaFinalStat = a->status;
  if (a->next > otaAckNext) otaAckNext = a->next;
}

bool coreOtaStart(uint8_t nodeIdx) {
  if (coreOtaBusy()) return false;
  ota.node  = (int8_t)nodeIdx;
  ota.phase = CoreOtaPhase::Failed;      // until proven otherwise; keeps msg visible
  ota.endMs = millis();
  if (!meshCoreActive || nodeIdx >= CORE_MAX_NODES || !coreNodes[nodeIdx].active) {
    coreOtaSetMsg("NODE NOT CONNECTED"); return false;
  }
  // Pick the image matching this node's chip. The roster only records band
  // (dual-band vs 2.4 GHz-only), which maps 1:1 to node chip in the current board
  // set: dual-band = C5 worker, 2.4-only = C3 OLED node. If the node's chip were
  // ever mismatched, the node's own esp_ota/SHA check rejects it (keeps old fw).
  const uint16_t wantChip = coreNodes[nodeIdx].band24Only ? OTA_CHIP_ESP32C3 : OTA_CHIP_ESP32C5;
  CoreOtaImage* im = nullptr;
  for (int i = 0; i < OTA_IMAGE_COUNT; i++)
    if (otaImages[i].chip == wantChip) { im = &otaImages[i]; break; }
  if (!im || !im->image) {
    coreOtaSetMsg(im && im->failMsg[0] ? im->failMsg : "NO IMAGE FOR NODE");
    return false;
  }
  // Point the active transfer at the chosen preloaded image (not owned here).
  ota.image  = im->image;
  ota.size   = im->size;
  ota.chunks = im->chunks;
  memcpy(ota.sha, im->sha, 32);

  memcpy(ota.mac, coreNodes[nodeIdx].mac, 6);
  ota.session  = (uint8_t)(esp_random() & 0xFF);
  ota.base     = ota.nextSend = 0;
  otaGotReady  = false;
  otaAckNext   = 0;
  otaFinalStat = 0xFF;
  coreOtaSetMsg("");
  ota.startMs     = millis();
  ota.lastBeginMs = 0;
  ota.phase       = CoreOtaPhase::Starting;
  Serial.printf("[OTA] node %u: %lu bytes, %u chunks, session %u\n", (unsigned)nodeIdx,
                (unsigned long)ota.size, (unsigned)ota.chunks, (unsigned)ota.session);
  return true;
}

void coreOtaAbort() {
  if (!coreOtaBusy()) return;
  coreOtaClose(CoreOtaPhase::Failed, "CANCELLED");   // the node abandons an idle update after 15 s
}

CoreOtaStatus coreOtaStatus() {
  CoreOtaStatus st = {};
  st.phase      = ota.phase;
  st.nodeIdx    = ota.node;
  st.imageBytes = ota.image ? ota.size : 0;
  st.totalBytes = ota.size;
  uint32_t b    = (uint32_t)ota.base * PIG_OTA_CHUNK;
  st.sentBytes  = b > ota.size ? ota.size : b;
  strlcpy(st.msg, ota.msg, sizeof(st.msg));
  strlcpy(st.loadMsg, ota.loadMsg, sizeof(st.loadMsg));
  return st;
}

static bool coreOtaSendBegin() {
  pig_ota_begin_t b = {};
  memcpy(b.magic, JCMK_MAGIC, 4);
  b.type    = PIG_MSG_OTA_BEGIN;
  b.session = ota.session;
  b.size    = ota.size;
  b.chunks  = ota.chunks;
  memcpy(b.sha256, ota.sha, 32);
  return esp_now_send(ota.mac, (uint8_t*)&b, sizeof(b)) == ESP_OK;
}

// Returns false if the chunk could not be sent right now (retry next tick).
static bool coreOtaSendChunk(uint16_t seq) {
  pig_ota_data_t d;
  memcpy(d.magic, JCMK_MAGIC, 4);
  d.type    = PIG_MSG_OTA_DATA;
  d.session = ota.session;
  d.seq     = seq;
  uint32_t off = (uint32_t)seq * PIG_OTA_CHUNK;
  uint32_t n   = ota.size - off;
  if (n > PIG_OTA_CHUNK) n = PIG_OTA_CHUNK;
  memcpy(d.data, ota.image + off, n);
  d.len = (uint8_t)n;
  return esp_now_send(ota.mac, (uint8_t*)&d, offsetof(pig_ota_data_t, data) + n) == ESP_OK;
}

static const char* coreOtaErrText(uint8_t st) {
  switch (st) {
    case PIG_OTA_ST_ERR_SPACE: return "NODE HAS NO OTA SLOT";   // old flash layout: needs one USB reflash
    case PIG_OTA_ST_ERR_WRITE: return "NODE FLASH WRITE ERR";
    case PIG_OTA_ST_ERR_SHA:   return "NODE: SHA MISMATCH";
    default:                   return "NODE REJECTED UPDATE";
  }
}

static void coreOtaTick() {
  uint32_t now = millis();
  if (!coreOtaBusy()) {
    if ((ota.phase == CoreOtaPhase::Done || ota.phase == CoreOtaPhase::Failed) &&
        now - ota.endMs > OTA_RESULT_SHOW_MS) {
      ota.phase = CoreOtaPhase::Idle;
      ota.node  = -1;
    }
    return;
  }
  if (ota.node < 0 || !coreNodes[ota.node].active) {
    coreOtaClose(CoreOtaPhase::Failed, "NODE LOST");
    return;
  }

  switch (ota.phase) {
    case CoreOtaPhase::Starting:
      if (otaFinalStat != 0xFF && otaFinalStat != PIG_OTA_ST_DONE) {
        coreOtaClose(CoreOtaPhase::Failed, coreOtaErrText(otaFinalStat));
      } else if (otaGotReady) {
        ota.phase = CoreOtaPhase::Sending;
        ota.base = ota.nextSend = 0;
        ota.ackMs = ota.progressMs = now;
        Serial.println("[OTA] node ready — sending image");
      } else if (now - ota.startMs > OTA_START_TIMEOUT_MS) {
        coreOtaClose(CoreOtaPhase::Failed, "NODE DID NOT REPLY");
      } else if (ota.lastBeginMs == 0 || now - ota.lastBeginMs >= OTA_BEGIN_RETRY_MS) {
        ota.lastBeginMs = now;
        coreOtaSendBegin();
      }
      break;

    case CoreOtaPhase::Sending: {
      uint8_t fin = otaFinalStat;
      if (fin == PIG_OTA_ST_DONE) { ota.base = ota.chunks; coreOtaClose(CoreOtaPhase::Done, "UPDATED - REBOOTING"); return; }
      if (fin != 0xFF)            { coreOtaClose(CoreOtaPhase::Failed, coreOtaErrText(fin)); return; }

      uint16_t acked = otaAckNext;
      if (acked > ota.chunks) acked = ota.chunks;
      if (acked > ota.base) { ota.base = acked; ota.ackMs = ota.progressMs = now; }
      if (ota.nextSend < ota.base) ota.nextSend = ota.base;

      if (now - ota.progressMs > OTA_STALL_MS) { coreOtaClose(CoreOtaPhase::Failed, "TRANSFER STALLED"); return; }
      // Lost frame or ack: go back to the first unacknowledged chunk.
      if (ota.nextSend > ota.base && now - ota.ackMs > OTA_ACK_TIMEOUT_MS) {
        ota.nextSend = ota.base;
        ota.ackMs    = now;
      }
      while (ota.nextSend < ota.chunks && ota.nextSend < (uint16_t)(ota.base + PIG_OTA_WINDOW)) {
        if (!coreOtaSendChunk(ota.nextSend)) break;   // busy radio -> retry next tick
        ota.nextSend++;
      }
      break;
    }
    default: break;
  }
}

// Send Biscuit Node a role assignment (MSG_ROLE_ASSIGN, type=5) then a channel
// config (MSG_CONFIG_UPDATE, type=10). Both packets must be full size (212 bytes).
// Called from coreReassignChannels / coreResendAdminToAll for isBiscuit nodes.
static void coreSendBiscuitChannels(const uint8_t* mac, const uint8_t* chans, uint8_t n) {
  if (n == 0) return;
  // Step 1: role assignment — text[0] = 1 (ROLE_WIFI). Biscuit MSG_ROLE_ASSIGN
  // = type 5. After receiving this the node transitions to STATE_SCANNING.
  jcmk_text_msg_t roleMsg = {};
  memcpy(roleMsg.magic, JCMK_MAGIC, 4);
  roleMsg.type    = JCMK_MSG_ADMIN;
  roleMsg.counter = 0;
  roleMsg.len     = 1;
  roleMsg.text[0] = 1;  // ROLE_WIFI
  esp_now_send(mac, (uint8_t*)&roleMsg, sizeof(roleMsg));
  delay(10);

  // Step 2: channel config — "channels=<explicit list>;dwell=N" (MSG_CONFIG_UPDATE,
  // type 10). The list is the exact set the node should scan — sparse (e.g. 1,6,11)
  // for 2.4-only nodes, a 5 GHz slice for dual-band nodes.
  jcmk_text_msg_t cfgMsg = {};
  memcpy(cfgMsg.magic, JCMK_MAGIC, 4);
  cfgMsg.type    = 10;
  cfgMsg.counter = 0;

  String chList = "channels=";
  for (uint8_t i = 0; i < n; i++) {
    if (i) chList += ",";
    chList += String(chans[i]);
  }
  chList += ";dwell=";
  chList += String((uint32_t)MESH_NODE_DWELL_MS);

  uint16_t slen = (chList.length() < JCMK_TEXT_MAX)
                ? (uint16_t)chList.length() : (uint16_t)JCMK_TEXT_MAX;
  cfgMsg.len = slen;
  memcpy(cfgMsg.text, chList.c_str(), slen);
  cfgMsg.text[slen] = '\0';
  esp_now_send(mac, (uint8_t*)&cfgMsg, sizeof(cfgMsg));

  Serial.printf("[CORE] cfg -> %02X%02X: %s\n", mac[4], mac[5], chList.c_str());
}

// ---- Biscuit plain-reply (encrypt=0) node-side interop helpers ----

// True if a received CORE_REPLY indicates a Biscuit Core running in
// unencrypted-reply mode (carries "encrypt=0" somewhere in its text field).
static bool biscuitHasPlainReply(const void* data, int len) {
  if (len < (int)sizeof(jcmk_text_msg_t)) return false;
  const jcmk_text_msg_t* msg = (const jcmk_text_msg_t*)data;
  size_t n = msg->len < JCMK_TEXT_MAX ? msg->len : JCMK_TEXT_MAX;
  for (size_t i = 0; i + 9 <= n; i++) {
    if (memcmp(msg->text + i, "encrypt=0", 9) == 0) return true;
  }
  return false;
}

// Parses a Biscuit "channels=1,2,...;dwell=N" config message into
// biscuitChannels[]/biscuitChannelCount, restarting the scan cycle from the
// new list. Only used for plain-reply Biscuit pairings.
static void biscuitSetChannels(const jcmk_text_msg_t* msg) {
  if (msg->len < 9 || msg->len > JCMK_TEXT_MAX || memcmp(msg->text, "channels=", 9)) {
    biscuitChannelCount = 0;
    return;
  }
  char text[JCMK_TEXT_MAX + 1];
  memcpy(text, msg->text, msg->len);
  text[msg->len] = '\0';
  char* end = strchr(text, ';');
  if (end) *end = '\0';
  uint8_t channels[JCMK_NUM_CHANNELS + 2];
  uint8_t count = 0;
  for (char* token = text + 9; *token && count < JCMK_NUM_CHANNELS + 2;) {
    char* next;
    long channel = strtol(token, &next, 10);
    if (next == token || channel < 1 || channel > 255 || (*next && *next != ',')) {
      count = 0;
      break;
    }
    if (channel == 104 || channel == 108) {
      channels[count++] = (uint8_t)channel;
    } else {
      for (uint8_t i = 0; i < JCMK_NUM_CHANNELS; i++) {
        if (JCMK_CHANNELS[i] == channel) { channels[count++] = (uint8_t)channel; break; }
      }
    }
    if (!*next) break;
    token = next + 1;
  }
  memcpy(biscuitChannels, channels, count);
  biscuitChannelCount = count;
  pig_sweep_invalidate(&nodeSweep);   // restart the scan cycle on the new list
}

// Generic Biscuit protocol-control reply, always unicast to the Core. Used
// for the type=15 (admin ack) and type=11 (config ack) handshake messages a
// plain-reply Biscuit Core expects during node registration.
static void biscuitSendControl(uint8_t type, uint32_t counter = 0, const char* text = nullptr) {
  jcmk_text_msg_t msg = {};
  memcpy(msg.magic, JCMK_MAGIC, 4);
  msg.type = type;
  msg.counter = counter;
  if (text) {
    msg.len = strlen(text);
    memcpy(msg.text, text, msg.len);
  }
  esp_now_send(jcmkCoreMac, (uint8_t*)&msg, sizeof(msg));
}

// ESP-Now receive callback — handles both Node and Core roles
static void jcmkOnRecv(const esp_now_recv_info_t* info,
                        const uint8_t* data, int len) {
  if (len < 5) return;
  if (data[0] != 'E' || data[1] != 'N' || data[2] != 'O' || data[3] != 'W') return;
  uint8_t type = data[4];

  if (meshCoreActive) {
    // ---- Core role: handle requests from nodes ----
  if (type == JCMK_MSG_CORE_REQUEST) {
      Serial.printf("[CORE] RX CORE_REQUEST from %02X:%02X:%02X:%02X:%02X:%02X len=%d\n",
        info->src_addr[0],info->src_addr[1],info->src_addr[2],
        info->src_addr[3],info->src_addr[4],info->src_addr[5], len);
      coreSendReply(info->src_addr);  // reply immediately (safe from callback)
      uint8_t next = (coreReqTail + 1) % CORE_REQ_QUEUE;
      if (next != coreReqHead) {
        memcpy(coreReqBuf[coreReqTail].mac, info->src_addr, 6);
        // Biscuit Node always sends full-size packets (212 bytes); JCMK sends 5 bytes.
        coreReqBuf[coreReqTail].isBiscuit = (len >= (int)sizeof(jcmk_text_msg_t));
        coreReqBuf[coreReqTail].band24    = jcmkPacketIsBand24(data, len);
        coreReqTail = next;
      }
#if PIG_FEAT_CORE_ISO
      else { gRxReqDrops++; }   // req ring full: node registration lost (RX miss)
#endif
    } else if (type == JCMK_MSG_TEXT && len >= 11) {
      const jcmk_text_msg_t* tm = (const jcmk_text_msg_t*)data;
      uint8_t next = (coreTextTail + 1) % CORE_TEXT_QUEUE;
      if (next != coreTextHead) {
        uint16_t slen = (tm->len < JCMK_TEXT_MAX) ? tm->len : JCMK_TEXT_MAX;
        memcpy(coreTextBuf[coreTextTail].line, tm->text, slen);
        coreTextBuf[coreTextTail].line[slen] = '\0';
#if PIG_FEAT_PAIR_SCHED
        // Tag the record with its reporting node so the drain can attribute it to an
        // antenna-pair member for the overlap metric (0xFF until the node registers).
        uint8_t sn = 0xFF;
        for (uint8_t q = 0; q < CORE_MAX_NODES; q++)
          if (coreNodes[q].active && memcmp(coreNodes[q].mac, info->src_addr, 6) == 0) { sn = q; break; }
        coreTextBuf[coreTextTail].srcNode = sn;
#endif
        coreTextTail = next;
      }
#if PIG_FEAT_CORE_ISO
      else { gRxTextDrops++; }  // text ring full: scan record lost (RX miss)
#endif
      // Update heartbeat for known node; queue unknown node for registration.
      // This handles reconnect: node has jcmkHaveCore=true and skips CORE_REQUEST,
      // but the Core can register it from any received packet (JCMK touchNode pattern).
      {
        bool found = false;
        for (uint8_t i = 0; i < CORE_MAX_NODES; i++) {
          if (coreNodes[i].active && memcmp(coreNodes[i].mac, info->src_addr, 6) == 0) {
            coreNodes[i].lastHbMs = millis();
            coreNodes[i].recordsRx++;
            found = true; break;
          }
        }
        if (!found) {
          uint8_t nxt = (coreReqTail + 1) % CORE_REQ_QUEUE;
          if (nxt != coreReqHead) {
            memcpy(coreReqBuf[coreReqTail].mac, info->src_addr, 6);
            coreReqBuf[coreReqTail].isBiscuit = (len >= (int)sizeof(jcmk_text_msg_t));
            coreReqBuf[coreReqTail].band24    = jcmkPacketIsBand24(data, len);
            coreReqTail = nxt;
          }
        }
      }
    } else if (type == JCMK_MSG_HEARTBEAT) {
      // Update heartbeat for known node; queue unknown node for registration.
      bool found = false;
      for (uint8_t i = 0; i < CORE_MAX_NODES; i++) {
        if (coreNodes[i].active && memcmp(coreNodes[i].mac, info->src_addr, 6) == 0) {
          coreNodes[i].lastHbMs = millis();
          found = true; break;
        }
      }
      if (!found) {
        uint8_t nxt = (coreReqTail + 1) % CORE_REQ_QUEUE;
        if (nxt != coreReqHead) {
          memcpy(coreReqBuf[coreReqTail].mac, info->src_addr, 6);
          coreReqBuf[coreReqTail].isBiscuit = (len >= (int)sizeof(jcmk_text_msg_t));
          coreReqBuf[coreReqTail].band24    = jcmkPacketIsBand24(data, len);
          coreReqTail = nxt;
        }
      }
    } else if (type == PIG_MSG_OTA_ACK) {
      coreOtaOnAck(info->src_addr, data, len);
    }
  } else {
    // ---- Node role (existing) ----
    if (type == JCMK_MSG_CORE_REPLY && !jcmkHaveCore && !jcmkCoreFoundPending) {
      memcpy(jcmkCoreMacPending, info->src_addr, 6);
      biscuitPlainReplyPending = biscuitHasPlainReply(data, len);
      jcmkCoreFoundPending = true;
    } else if (biscuitPlainReplyPending && jcmkHaveCore
               && memcmp(info->src_addr, jcmkCoreMac, 6) == 0
               && len >= (int)sizeof(jcmk_text_msg_t) && type == JCMK_MSG_ADMIN) {
      // Plain-reply Biscuit admin handshake step: ack with type=15, echoing
      // back the first text byte (or low byte of counter as a fallback).
      // Fully gated on biscuitPlainReplyPending so a WarHog/JCMK Core's own
      // 212-byte role message still falls through to the binary-admin branch
      // below exactly as before.
      const jcmk_text_msg_t* msg = (const jcmk_text_msg_t*)data;
      biscuitSendControl(15, msg->len ? (uint8_t)msg->text[0] : msg->counter & 0xFF);
    } else if (biscuitPlainReplyPending && jcmkHaveCore
               && memcmp(info->src_addr, jcmkCoreMac, 6) == 0
               && len >= (int)sizeof(jcmk_text_msg_t) && type == 10) {
      // Plain-reply Biscuit channel-config message (MSG_CONFIG_UPDATE) --
      // parse the assigned channel list, then ack with type=11.
      biscuitSetChannels((const jcmk_text_msg_t*)data);
      biscuitSendControl(11);
    } else if (type == PIG_MSG_ADMIN_EXT && len >= (int)pig_admin_ext_len(0)) {
      // Phase 3: explicit yield-packed channel list from a WarHog Core.
      const pig_admin_ext_t* ax = (const pig_admin_ext_t*)data;
      if (ax->assign_ver != jcmkAssignVer) {
        jcmkAssignVer = ax->assign_ver;
        uint8_t avail = (uint8_t)(len - (int)pig_admin_ext_len(0));
        uint8_t cnt = ax->n; if (cnt > avail) cnt = avail; if (cnt > JCMK_NUM_CHANNELS) cnt = JCMK_NUM_CHANNELS;
        for (uint8_t i = 0; i < cnt; i++) biscuitChannels[i] = ax->channels[i];
        biscuitChannelCount = cnt;
        pig_sweep_invalidate(&nodeSweep);   // restart the scan cycle on the new list
      }
    } else if (type == JCMK_MSG_ADMIN && len >= (int)sizeof(jcmk_admin_msg_t)) {
      const jcmk_admin_msg_t* adm = (const jcmk_admin_msg_t*)data;
      if (adm->assignment_version != jcmkAssignVer) {
        jcmkAssignVer = adm->assignment_version;
        jcmkStartIdx  = adm->start_channel_idx;
        jcmkEndIdx    = adm->end_channel_idx;
        pig_sweep_invalidate(&nodeSweep);   // restart the scan cycle on the new range
      }
    }
  }
}

// ================================================================
//  Core mode helpers (main loop only — not ISR-safe)
// ================================================================
static void coreFindOrAddNode(const uint8_t* mac, bool isBiscuit, bool band24) {
  for (uint8_t i = 0; i < CORE_MAX_NODES; i++) {
    if (coreNodes[i].active && memcmp(coreNodes[i].mac, mac, 6) == 0) {
      coreNodes[i].lastHbMs = millis();
      coreNodes[i].band24Only = band24;  // pick up a (re)declared band; caller reassigns
      return;  // already registered
    }
  }
  for (uint8_t i = 0; i < CORE_MAX_NODES; i++) {
    if (!coreNodes[i].active) {
      coreNodes[i].active    = true;
      coreNodes[i].lastHbMs  = millis();
      coreNodes[i].recordsRx = 0;
      coreNodes[i].isBiscuit = isBiscuit;
      coreNodes[i].band24Only = band24;
      coreNodes[i].group     = rememberedGroup(mac);
      coreNodes[i].chanCount = 0;
      memcpy(coreNodes[i].mac, mac, 6);
      coreNodeCount++;
      jcmkAddPeer(mac);
      // Re-send CORE_REPLY now that the peer is registered.
      // The initial reply from jcmkOnRecv() may have failed because the node's
      // MAC wasn't yet in the peer list when esp_now_send() was called.
      coreSendReply(mac);
      Serial.printf("[CORE] New %s node %d: %02X:%02X:%02X:%02X:%02X:%02X\n",
        isBiscuit ? "Biscuit" : "JCMK",
        i, mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
      return;
    }
  }
  Serial.println("[CORE] Max nodes reached");
}

// Record the exact channel list a node is scanning (for the on-device UI) and
// push it to the node.
static void coreApplyNodeChannels(CoreNodeInfo& nd, const uint8_t* list, uint8_t nl) {
  nd.chanCount = (nl <= sizeof(nd.chans)) ? nl : (uint8_t)sizeof(nd.chans);
  memcpy(nd.chans, list, nd.chanCount);
  coreSendBiscuitChannels(nd.mac, list, nl);
}

// Band-aware channel assignment for ONE sub-cluster. 2.4 GHz-only nodes
// (declared via "B24") scan the productive 2.4 set {1,6,11}; dual-band (C5)
// nodes split the 5 GHz space. If the group has no dual nodes, 5 GHz simply
// isn't covered (nothing can scan it); if it has no 2.4-only nodes, the first
// dual node also takes {1,6,11} so 2.4 stays covered. Splitting happens inside
// the group so co-located nodes never scan the same channel at once; a group
// of one node therefore scans everything. `idx`/`n` are indices into
// coreNodes[] of the active members of this group. All of the user's nodes are
// Biscuit-type (full-size packets), so every node gets an explicit
// "channels=..." string config.
static void coreAssignGroup(const uint8_t* idx, uint8_t n) {
  if (n == 0) return;

#if PIG_FEAT_PAIR_SCHED
  // Phase 5: antenna-pair complementary assignment. Each group IS one antenna; its
  // members take alternating roles (A/B, by order within the group) and get the
  // disjoint role channel lists from pig_dwell.h, phase-offset per group so the six
  // pairs don't scan in lockstep. 2.4-only nodes take their role's 2.4 subset. DFS is
  // handed out as a small rotating complementary pair (passive) to approximate the
  // ~3-4 s DFS revisit without flooding every sweep. Per-channel dwell is applied
  // node-side (pig_pair_plan); the Core only owns the channel-list split here.
  {
    static uint8_t pairDfsRot = 0;
    const uint8_t g = coreNodes[idx[0]].group;

    uint8_t dfs[PIG_MAX_CH]; uint8_t ndfs = 0;
    for (uint8_t j = 0; j < JCMK_NUM_CHANNELS; j++)
      if (JCMK_CHANNELS[j] >= 52 && JCMK_CHANNELS[j] <= 144) dfs[ndfs++] = JCMK_CHANNELS[j];

    for (uint8_t t = 0; t < n; t++) {
      CoreNodeInfo& nd = coreNodes[idx[t]];
      const uint8_t role    = (uint8_t)(t & 1);                 // 0 = A, 1 = B
      const uint8_t* rlist  = role ? PIG_PAIR_LIST_B : PIG_PAIR_LIST_A;
      const uint8_t  rn     = role ? PIG_PAIR_LIST_B_N : PIG_PAIR_LIST_A_N;
      const uint8_t  off    = (uint8_t)((g >= 1 ? g - 1 : 0) % rn);  // per-group phase offset

      uint8_t list[PIG_ADMIN_EXT_MAXCH]; uint8_t nl = 0;
      for (uint8_t j = 0; j < rn && nl < (uint8_t)sizeof(list); j++) {
        const uint8_t ch = rlist[(off + j) % rn];
        if (nd.band24Only && ch > 14) continue;                 // 2.4-only node: skip 5 GHz
        list[nl++] = ch;
      }
      if (!nd.band24Only && ndfs > 0 && nl < (uint8_t)sizeof(list)) {
        const uint8_t base = (uint8_t)((pairDfsRot * 2 + role) % ndfs);  // rotating, complementary
        list[nl++] = dfs[base];
        if (ndfs > 1 && nl < (uint8_t)sizeof(list)) list[nl++] = dfs[(base + 2) % ndfs];
      }
      if (nl == 0) { list[nl++] = 1; list[nl++] = 6; list[nl++] = 11; }   // safety
      nd.startIdx = 0; nd.endIdx = (uint8_t)(nl ? nl - 1 : 0);            // display only
      coreApplyNodeChannels(nd, list, nl);
      delay(10);
    }
    pairDfsRot++;
    return;
  }
#endif

  uint8_t dual[CORE_MAX_NODES], only24[CORE_MAX_NODES];
  uint8_t nDual = 0, n24 = 0;
  for (uint8_t t = 0; t < n; t++) {
    uint8_t i = idx[t];
    if (coreNodes[i].band24Only) only24[n24++] = i;
    else                         dual[nDual++] = i;
  }

  static const uint8_t CH24[3] = {1, 6, 11};
  const uint8_t* ch5 = &JCMK_CHANNELS[14];                 // ch 36..177
  const uint8_t  n5  = (uint8_t)(JCMK_NUM_CHANNELS - 14);  // 26 channels

  // 2.4-only nodes: each scans 1/6/11.
  for (uint8_t k = 0; k < n24; k++) {
    CoreNodeInfo& nd = coreNodes[only24[k]];
    nd.startIdx = 0;  nd.endIdx = 10;  // display only
    coreApplyNodeChannels(nd, CH24, 3);
    delay(10);
  }

  // Dual-band nodes: split the 5 GHz channels into slices.
  if (nDual > 0) {
#if PIG_FEAT_DYNAMIC_ASSIGN
    // Phase 3: reorder the 5 GHz channels by yield so productive ones are placed
    // first; the slicing below then front-loads high-yield channels.
    uint8_t ch5ord[PIG_MAX_CH];
    { uint8_t schans[PIG_MAX_CH], rank[PIG_MAX_CH]; ChanStat sstat[PIG_MAX_CH];
      for (uint8_t j = 0; j < n5; j++) { schans[j] = ch5[j]; sstat[j] = gChanStat[14 + j]; }
      sched_rank(sstat, schans, n5, millis(), gSchedCfg, rank);
      for (uint8_t j = 0; j < n5; j++) ch5ord[j] = schans[rank[j]];
    }
    const uint8_t* ch5use = ch5ord;
#else
    const uint8_t* ch5use = ch5;
#endif
    uint8_t per = (nDual <= n5) ? (uint8_t)(n5 / nDual) : 1;
    if (per == 0) per = 1;
    for (uint8_t k = 0; k < nDual; k++) {
      CoreNodeInfo& nd = coreNodes[dual[k]];
      uint8_t list[32];
      uint8_t nl = 0;
      if (n24 == 0 && k == 0) { list[nl++] = 1; list[nl++] = 6; list[nl++] = 11; }
      uint8_t s = (uint8_t)(k * per);
      if (s < n5) {
        uint8_t e = (k == nDual - 1) ? (uint8_t)(n5 - 1) : (uint8_t)(s + per - 1);
        if (e >= n5) e = (uint8_t)(n5 - 1);
        for (uint8_t j = s; j <= e && nl < sizeof(list); j++) list[nl++] = ch5use[j];
        nd.startIdx = (uint8_t)(14 + s);
        nd.endIdx   = (uint8_t)(14 + e);
      }
      if (nl == 0) { list[nl++] = 1; list[nl++] = 6; list[nl++] = 11; }  // safety
      coreApplyNodeChannels(nd, list, nl);
      delay(10);
    }
  }
}

// Assign channels across every sub-cluster. Each group is balanced on its own
// (see coreAssignGroup), so groups overlap in coverage but members of the same
// group never do.
static void coreAssignAndSend(bool bumpVersion) {
  if (coreNodeCount == 0) return;
  if (bumpVersion) coreAssignVer++;

  uint8_t nGroups = 0;
  for (uint8_t g = 1; g <= CORE_MAX_GROUPS; g++) {
    uint8_t idx[CORE_MAX_NODES];
    uint8_t n = 0;
    for (uint8_t i = 0; i < CORE_MAX_NODES; i++)
      if (coreNodes[i].active && coreNodes[i].group == g) idx[n++] = i;
    if (n == 0) continue;
    coreAssignGroup(idx, n);
    nGroups++;
  }

  Serial.printf("[CORE] Sub-cluster assign: %u node(s) across %u group(s), v%d\n",
                (unsigned)coreNodeCount, (unsigned)nGroups, coreAssignVer);
}

static void coreReassignChannels() { coreAssignAndSend(true);  }

// Re-send the current assignment to every node (nodes that missed an update
// mid-scan recover). Does not bump the version.
static void coreResendAdminToAll() { if (coreNodeCount) coreAssignAndSend(false); }

// ================================================================
//  Sub-cluster control + saved wardriving profiles (public API)
// ================================================================
uint8_t coreGroupMemberCount(uint8_t group) {
  uint8_t n = 0;
  for (uint8_t i = 0; i < CORE_MAX_NODES; i++)
    if (coreNodes[i].active && coreNodes[i].group == group) n++;
  return n;
}

void coreSetNodeGroup(uint8_t nodeIdx, uint8_t group) {
  if (nodeIdx >= CORE_MAX_NODES || !coreNodes[nodeIdx].active) return;
  if (group < 1) group = 1;
  if (group > CORE_MAX_GROUPS) group = CORE_MAX_GROUPS;
  if (coreNodes[nodeIdx].group == group) return;
  coreNodes[nodeIdx].group = group;
  rememberGroup(coreNodes[nodeIdx].mac, group);
  coreReassignChannels();   // bump version + resend to every node
}

// ---- Wardriving profiles: named node→sub-cluster presets, saved on SD so a
// recurring physical setup (same boards, same antennas) can be recalled with
// one tap instead of re-grouping every session. Stored as a small text file. --
static const char* MESH_PROFILE_PATH = "/mesh_profiles.cfg";

struct MeshProfileEntry { uint8_t mac[6]; uint8_t group; };
struct MeshProfile {
  bool            used;
  char            name[MESH_PROFILE_NAME_MAX + 1];
  uint8_t         count;
  MeshProfileEntry entry[CORE_MAX_NODES];
};
static MeshProfile meshProfiles[MESH_NUM_PROFILES] = {};
static bool        meshProfilesLoaded = false;

static void meshParseMac(const String& s, uint8_t out[6]) {
  for (uint8_t i = 0; i < 6; i++)
    out[i] = (uint8_t)strtol(s.substring(i * 2, i * 2 + 2).c_str(), nullptr, 16);
}

static void meshProfilesLoad() {
  memset(meshProfiles, 0, sizeof(meshProfiles));
  meshProfilesLoaded = true;
  if (!sdOk) return;
  // Hold the SD lock or the read collides with the core-0 SD-log task and
  // silently returns nothing.
  spiBusLock();
  if (!SD.exists(MESH_PROFILE_PATH)) { spiBusUnlock(); return; }
  SdReader f(MESH_PROFILE_PATH);       // DMA-safe read (see SDUtils.h)
  if (!f) { spiBusUnlock(); return; }
  int slot = -1;
  while (f.available()) {
    String line = f.readStringUntil('\n');
    line.trim();
    if (line.length() == 0 || line[0] == '#') continue;
    if (line[0] == 'P') {                              // "P <slot> <name...>"
      int sp1 = line.indexOf(' ');
      int sp2 = line.indexOf(' ', sp1 + 1);
      if (sp1 < 0) { slot = -1; continue; }
      slot = line.substring(sp1 + 1, sp2 < 0 ? line.length() : sp2).toInt();
      if (slot < 0 || slot >= MESH_NUM_PROFILES) { slot = -1; continue; }
      String nm = (sp2 < 0) ? String("") : line.substring(sp2 + 1);
      meshProfiles[slot].used  = true;
      meshProfiles[slot].count = 0;
      strncpy(meshProfiles[slot].name, nm.c_str(), MESH_PROFILE_NAME_MAX);
      meshProfiles[slot].name[MESH_PROFILE_NAME_MAX] = '\0';
    } else if (line[0] == 'N' && slot >= 0) {          // "N <AABBCCDDEEFF> <group>"
      int sp1 = line.indexOf(' ');
      int sp2 = line.indexOf(' ', sp1 + 1);
      if (sp1 < 0 || sp2 < 0) continue;
      String macStr = line.substring(sp1 + 1, sp2);
      uint8_t g = (uint8_t)line.substring(sp2 + 1).toInt();
      MeshProfile& p = meshProfiles[slot];
      if (macStr.length() >= 12 && p.count < CORE_MAX_NODES) {
        meshParseMac(macStr, p.entry[p.count].mac);
        p.entry[p.count].group = (g < 1) ? 1 : (g > CORE_MAX_GROUPS ? CORE_MAX_GROUPS : g);
        p.count++;
      }
    }
  }
  f.close();
  spiBusUnlock();
}

static void meshProfilesSaveToSD() {
  if (!sdOk) return;
  spiBusLock();   // same shared-SPI-bus rule as saveConfigToSD()
  if (SD.exists(MESH_PROFILE_PATH)) SD.remove(MESH_PROFILE_PATH);
  File f = SD.open(MESH_PROFILE_PATH, FILE_WRITE);
  if (!f) {
    spiBusUnlock();
    Serial.println("[PROFILE] Failed to open " "/mesh_profiles.cfg for write");
    return;
  }
  f.println("# warhog mesh profiles v1");
  for (uint8_t s = 0; s < MESH_NUM_PROFILES; s++) {
    if (!meshProfiles[s].used) continue;
    f.printf("P %u %s\n", (unsigned)s, meshProfiles[s].name);
    for (uint8_t e = 0; e < meshProfiles[s].count; e++) {
      const uint8_t* m = meshProfiles[s].entry[e].mac;
      f.printf("N %02X%02X%02X%02X%02X%02X %u\n",
               m[0], m[1], m[2], m[3], m[4], m[5],
               (unsigned)meshProfiles[s].entry[e].group);
    }
  }
  f.flush();
  f.close();
  spiBusUnlock();
  Serial.println("[PROFILE] Saved /mesh_profiles.cfg");
}

void meshProfilesInit() { if (!meshProfilesLoaded) meshProfilesLoad(); }

// DEFAULT profile: put every node back into sub-cluster 1 so the Core splits
// channels evenly across all connected nodes (band-aware). Also forgets saved
// per-MAC groupings so nodes that reconnect default to the even split too.
void meshApplyDefault() {
  groupMemCount = 0;
  for (uint8_t i = 0; i < CORE_MAX_NODES; i++)
    if (coreNodes[i].active) coreNodes[i].group = 1;
  if (coreNodeCount) coreReassignChannels();
}

bool        meshProfileUsed(uint8_t slot)      { return slot < MESH_NUM_PROFILES && meshProfiles[slot].used; }
const char* meshProfileName(uint8_t slot)      { return (slot < MESH_NUM_PROFILES) ? meshProfiles[slot].name : ""; }
uint8_t     meshProfileNodeCount(uint8_t slot) { return (slot < MESH_NUM_PROFILES) ? meshProfiles[slot].count : 0; }

// Capture the current node→sub-cluster grouping (all connected nodes) into a
// slot and persist it.
void meshSaveProfile(uint8_t slot, const char* name) {
  if (slot >= MESH_NUM_PROFILES) return;
  if (!meshProfilesLoaded) meshProfilesLoad();
  MeshProfile& p = meshProfiles[slot];
  memset(&p, 0, sizeof(p));
  p.used = true;
  if (name && *name) {
    strncpy(p.name, name, MESH_PROFILE_NAME_MAX);
    p.name[MESH_PROFILE_NAME_MAX] = '\0';
  } else {
    snprintf(p.name, sizeof(p.name), "Profile %u", (unsigned)(slot + 1));
  }
  for (uint8_t i = 0; i < CORE_MAX_NODES && p.count < CORE_MAX_NODES; i++) {
    if (!coreNodes[i].active) continue;
    memcpy(p.entry[p.count].mac, coreNodes[i].mac, 6);
    p.entry[p.count].group = coreNodes[i].group;
    p.count++;
  }
  meshProfilesSaveToSD();
}

// Apply a saved profile: set the sub-cluster of every matching connected node
// (and remember it by MAC so nodes not yet connected land in the right group
// when they join), then recompute + push assignments. Returns how many
// connected nodes matched.
uint8_t meshApplyProfile(uint8_t slot) {
  if (slot >= MESH_NUM_PROFILES || !meshProfiles[slot].used) return 0;
  MeshProfile& p = meshProfiles[slot];
  uint8_t applied = 0;
  for (uint8_t e = 0; e < p.count; e++) {
    rememberGroup(p.entry[e].mac, p.entry[e].group);
    for (uint8_t i = 0; i < CORE_MAX_NODES; i++) {
      if (coreNodes[i].active && memcmp(coreNodes[i].mac, p.entry[e].mac, 6) == 0) {
        coreNodes[i].group = p.entry[e].group;
        applied++;
      }
    }
  }
  if (coreNodeCount) coreReassignChannels();
  return applied;
}

void meshDeleteProfile(uint8_t slot) {
  if (slot >= MESH_NUM_PROFILES) return;
  memset(&meshProfiles[slot], 0, sizeof(meshProfiles[slot]));
  meshProfilesSaveToSD();
}

static void coreSendHeartbeatToAll() {
  // Broadcast a single heartbeat instead of per-node unicast.
  // At 12 nodes, unicast was 12 × 212-byte sends with 10 ms gaps = 120+ ms of
  // blocking channel time. One broadcast reaches all nodes simultaneously and
  // keeps ch 6 clear for incoming TEXT bursts.
  jcmk_text_msg_t msg = {};
  memcpy(msg.magic, JCMK_MAGIC, 4);
  msg.type    = JCMK_MSG_HEARTBEAT;
  msg.counter = ++coreHbCounter;
  // WarHog extension: carry the authoritative mesh size (total active nodes)
  // so every node can display the real herd count. Marker keeps it invisible
  // to stock JCMK/Biscuit nodes, which ignore heartbeat text.
  memcpy(msg.text, WARHOG_MARKER, WARHOG_MARKER_LEN);
  msg.text[WARHOG_MARKER_LEN] = (char)coreNodeCount;
  msg.len     = WARHOG_MARKER_LEN + 1;
  esp_now_send(JCMK_BCAST, (uint8_t*)&msg, sizeof(msg));
}

#if PIG_FEAT_PAIR_SCHED
// Pair-overlap metric (friend's handoff §Pair Overlap): within each antenna pair
// (group), how often one node reports a BSSID its pair-partner also saw in the last
// second — the same-second redundancy the 12-node rig suffered. Event-based (counts
// sightings, not set cardinality), which directly tracks "is the 2nd receiver
// duplicating the 1st." Reported per group every 5 s as both/either + pct.
struct PairSeen { uint32_t hash; uint32_t ms; };
static PairSeen (*gPairSeen)[16] = psramCalloc<PairSeen[16]>(CORE_MAX_NODES);
static uint8_t  gPairSeenHead[CORE_MAX_NODES] = {};
static uint32_t gPairBoth[CORE_MAX_GROUPS + 1]   = {};
static uint32_t gPairEither[CORE_MAX_GROUPS + 1] = {};
static uint32_t gPairLogMs = 0;

static uint32_t pairBssidHash(const char* b) {   // FNV-1a over the BSSID text
  uint32_t h = 2166136261u;
  for (const char* p = b; *p; ++p) { h ^= (uint8_t)*p; h *= 16777619u; }
  return h;
}

static void pairOverlapObserve(uint8_t src, const char* bssid) {
  if (src >= CORE_MAX_NODES || !coreNodes[src].active) return;
  uint8_t g = coreNodes[src].group;
  if (g > CORE_MAX_GROUPS) g = CORE_MAX_GROUPS;
  const uint32_t h = pairBssidHash(bssid);
  const uint32_t now = millis();
  gPairEither[g]++;
  bool both = false;
  for (uint8_t j = 0; j < CORE_MAX_NODES && !both; j++) {
    if (j == src || !coreNodes[j].active || coreNodes[j].group != coreNodes[src].group) continue;
    for (uint8_t k = 0; k < 16; k++)
      if (gPairSeen[j][k].hash == h && (now - gPairSeen[j][k].ms) <= 1000u) { both = true; break; }
  }
  if (both) gPairBoth[g]++;
  const uint8_t head = gPairSeenHead[src];
  gPairSeen[src][head].hash = h;
  gPairSeen[src][head].ms   = now;
  gPairSeenHead[src] = (uint8_t)((head + 1) % 16);
}
#endif

#if PIG_FEAT_PAIR_SCHED
static void coreParseAndLogText(const char* line, uint8_t srcNode) {
#else
static void coreParseAndLogText(const char* line) {
#endif
  // JCMK format: BSSID,SSID,AUTH,CHANNEL,RSSI,W
  String s(line);
  int p0 = s.indexOf(',');                if (p0 < 0) return;
  int p1 = s.indexOf(',', p0 + 1);       if (p1 < 0) return;
  int p2 = s.indexOf(',', p1 + 1);       if (p2 < 0) return;
  int p3 = s.indexOf(',', p2 + 1);       if (p3 < 0) return;

  String bssid = s.substring(0,      p0);
  String ssid  = s.substring(p0 + 1, p1);
  String auth  = s.substring(p1 + 1, p2);
  int    ch    = s.substring(p2 + 1, p3).toInt();
  int    rssi  = s.substring(p3 + 1).toInt();  // toInt() stops at next comma
#if PIG_FEAT_DYNAMIC_ASSIGN
  { int ci = chIndexOf((uint8_t)ch); if (ci >= 0) gChanHits[ci]++; }  // Phase 3: per-channel yield
#endif
#if PIG_FEAT_PAIR_SCHED
  pairOverlapObserve(srcNode, bssid.c_str());   // Phase 5: antenna-pair redundancy metric
#endif

  double lat = 0, lon = 0, altM = 0, accM = 0;
  if (gpsHasFix) {
    lat  = gps.location.lat();
    lon  = gps.location.lng();
    altM = gps.altitude.meters();
    accM = gps.hdop.hdop();
  }
#if PIG_FEAT_CORE_ISO
  coreStoreEnqueue(bssid, ssid, auth, iso8601NowUTC(), ch, rssi, lat, lon, altM, accM);  // SD write -> core-0 task
  // Non-RT half runs on core 1: hand the parsed AP to the loop (coreMirrorDrain) so the
  // phone-app push + UI telemetry only ever touch NimBLE/UI state from the loop's core.
  { uint8_t next = (uint8_t)((coreMirrorTail + 1) % 64);
    if (next != coreMirrorHead) {
      CoreMirrorRec& m = coreMirrorBuf[coreMirrorTail];
      snprintf(m.bssid, sizeof(m.bssid), "%s", bssid.c_str());
      snprintf(m.ssid,  sizeof(m.ssid),  "%s", ssid.c_str());
      snprintf(m.auth,  sizeof(m.auth),  "%s", auth.c_str());
      m.ch = (int16_t)ch; m.rssi = (int16_t)rssi;
      coreMirrorTail = next;
    }
    // ring full -> drop the MIRROR copy only; the SD record above is already safe.
  }
#else
  appendWigleRow(bssid, ssid, auth, iso8601NowUTC(), ch, rssi, lat, lon, altM, accM);
  // Mirror each AP to whichever phone app this Core is paired with, so it plots
  // in real time on the map (personality chosen by the CORE BLE APP setting).
  if (coreBleIsIosEsp) iosEspCompatPushAp(bssid.c_str(), ssid.c_str(), ch, rssi, auth.c_str());
  else                 biscuitCompatPushAp(bssid.c_str(), ssid.c_str(), ch, rssi, auth.c_str());
  coreRecordsRx++;

  // Roll the cluster's finds into the same counters solo wardriving uses, so the
  // HOME/STAT/TOOL screens reflect the whole mesh instead of showing zeros.
  if (ch >= 1 && ch <= 14)        networksFound2G++;
  else if (ch >= 32 && ch <= 177) networksFound5G++;
  else                            devicesFoundBle++;   // ch 0 => BLE record
  screenTelemetryObserve(ssid, bssid, rssi, ch);
  screenTelemetry.loggedRows++;
#endif
}

#if PIG_FEAT_CORE_ISO
// Core-1 half of the record path: drain the parsed-AP mirror ring the RX task filled,
// doing the phone-app push + UI telemetry here so NimBLE/screen state stays single-core.
static void coreMirrorDrain() {
  while (coreMirrorHead != coreMirrorTail) {
    const CoreMirrorRec& m = coreMirrorBuf[coreMirrorHead];
    if (coreBleIsIosEsp) iosEspCompatPushAp(m.bssid, m.ssid, m.ch, m.rssi, m.auth);
    else                 biscuitCompatPushAp(m.bssid, m.ssid, m.ch, m.rssi, m.auth);
    coreRecordsRx++;
    if (m.ch >= 1 && m.ch <= 14)        networksFound2G++;
    else if (m.ch >= 32 && m.ch <= 177) networksFound5G++;
    else                                devicesFoundBle++;
    screenTelemetryObserve(String(m.ssid), String(m.bssid), m.rssi, m.ch);
    screenTelemetry.loggedRows++;
    coreMirrorHead = (uint8_t)((coreMirrorHead + 1) % 64);
  }
}

// Core-0 RT task: owns the coreTextBuf drain so ESP-NOW records are serviced within a
// couple ms regardless of what the main loop is doing. Polls every 2 ms and empties the
// ring each wake; parse + SD-enqueue is the only work done on this core.
static void tRxDrain(void*) {
  while (gRxDrainRun) {
    uint32_t us = micros();
    if (gRtLastUs) { uint32_t g = us - gRtLastUs; if (g > gRtMaxUs) gRtMaxUs = g; if (g > 5000) gRtGt5++; }
    gRtLastUs = us;
    uint16_t occ = (uint16_t)((coreTextTail - coreTextHead + CORE_TEXT_QUEUE) % CORE_TEXT_QUEUE);
    if (occ > gRxQMax) gRxQMax = occ;
    while (coreTextHead != coreTextTail) {
      uint8_t i = coreTextHead;
      coreTextHead = (uint8_t)((coreTextHead + 1) % CORE_TEXT_QUEUE);
#if PIG_FEAT_PAIR_SCHED
      coreParseAndLogText(coreTextBuf[i].line, coreTextBuf[i].srcNode);
#else
      coreParseAndLogText(coreTextBuf[i].line);
#endif
    }
    vTaskDelay(pdMS_TO_TICKS(2));
  }
  gRxDrainTask = nullptr;
  vTaskDelete(nullptr);
}

static void coreRxDrainStart() {
  if (gRxDrainTask) return;
  gRxDrainRun = true;
  coreMirrorHead = coreMirrorTail = 0;
  // core 0, prio 3 (above tStore's 2 so records parse promptly, below WiFi).
  xTaskCreatePinnedToCore(tRxDrain, "tRxDrain", 6144, nullptr, 3, &gRxDrainTask, 0);
}

static void coreRxDrainStop() {
  if (!gRxDrainTask) return;
  gRxDrainRun = false;   // task self-deletes on its next wake
}
#endif

// ================================================================
//  Per-channel async scan (JCMK startNextNodeAssignedScan pattern).
//  Scans one assigned channel per call at NODE_SCAN_DWELL_MS dwell,
//  cycles through the full assigned range, then enters a ch-6 admin
//  window (heartbeat + NODE_ADMIN_WIN_MS) before the next cycle.
// ================================================================
static void nodeDoScanTick() {
  // Admin window: radio is on ch 6, heartbeat already sent, just waiting
  if (nodeScanAdminWin) {
    if (millis() - nodeScanAdminMs >= NODE_ADMIN_WIN_MS) {
      nodeScanAdminWin = false;   // next tick rebuilds the sweep for a new cycle
    }
    return;
  }

  if (!nodeScanActive) {
    // Phase 2: (re)build the two-pass sweep at the top of each cycle. The
    // assignment can change between cycles (node join/leave, or a Phase-3
    // explicit list), and channels this radio cannot scan — 5 GHz where
    // wardriverIsC5() is false, i.e. all non-C5 silicon — are dropped here
    // instead of burning a tick mid-sweep.
    if (!pig_sweep_valid(&nodeSweep)) {
      uint8_t chans[PIG_SWEEP_MAXCH];
      uint8_t n = pig_sweep_build(chans, biscuitChannels, biscuitChannelCount,
                                  JCMK_CHANNELS, JCMK_NUM_CHANNELS,
                                  jcmkStartIdx, jcmkEndIdx, wardriverIsC5());
      if (n == 0) return;   // nothing scannable assigned yet — retry next tick
      pig_sweep_begin(&nodeSweep, chans, n);
    }

    // Sweep complete (fast pass + any passive revisits) — enter admin window
    uint8_t channel; PigScanPlan plan;
    if (!pig_sweep_next(&nodeSweep, &channel, &plan)) {
      jcmkSetChannel(JCMK_ESPNOW_CH);
      while (GPSSerial.available()) gps.encode(GPSSerial.read());
      if (jcmkHaveCore) { jcmkSendHeartbeat(); jcmkLastHbMs = millis(); }
      nodeScanAdminWin = true;
      // Random jitter (0-200 ms) staggers admin windows so nodes with similar
      // channel counts don't all transmit on ch 6 at the same instant.
      nodeScanAdminMs  = millis() + (uint32_t)(esp_random() % 200);
      pig_sweep_invalidate(&nodeSweep);
      return;
    }

    // Async scan of this single channel only (no blocking), with the plan the
    // sweep picked: a short active fast pass over the whole assignment, then
    // longer PASSIVE revisits of the channels that actually yielded. DFS 5 GHz
    // is always passive. With the flag off every plan is the legacy fixed
    // active scan and there is no revisit pass, so behavior is unchanged.
    int16_t rc = WiFi.scanNetworks(/*async*/true, /*hidden*/true,
                                    plan.passive, plan.dwell_ms, channel);
    if (rc == WIFI_SCAN_RUNNING || rc == 0) {
      nodeScanActive = true;
    } else {
      pig_sweep_report(&nodeSweep, 0);  // channel failed — no yield, move on
    }
    return;
  }

  // Scan in progress — poll for completion
  int n = WiFi.scanComplete();
  if (n == WIFI_SCAN_RUNNING) return;

  // What this channel contributed, i.e. everything Phase-1 dedup did NOT
  // suppress. That is the channel's yield: it ranks the passive revisit pass,
  // and with dedup off it degrades to the raw AP count, which ranks as well.
  uint16_t sweepYield = 0;

  if (n > 0) {
    jcmkNetworksFound += (uint32_t)n;
    // Return to ch 6 before any ESP-Now send (JCMK sendBroadcastStringPlain pattern).
    // The scan left the radio on the scan channel; Core listens only on ch 6.
    jcmkSetChannel(JCMK_ESPNOW_CH);
    for (int i = 0; i < n; i++) {
      if (pigDedupSkip(i)) continue;   // Phase 1 dedup: skip redundant repeat (no-op unless flag)
      sweepYield++;                    // Phase 2 revisit ranking
      String bssid = WiFi.BSSIDstr(i);
      String ssid  = WiFi.SSID(i);
      String auth  = meshAuthModeToString(WiFi.encryptionType(i));
      // A plain-reply Biscuit Core expects the bracketed AuthMode token set
      // its own nodes emit; a WarHog/JCMK Core takes the bare form unchanged.
      if (biscuitPlainReplyPending) {
        if      (auth == "OPEN")     auth = "[OPEN]";
        else if (auth == "WEP")      auth = "[WEP]";
        else if (auth == "WPA")      auth = "[WPA_PSK]";
        else if (auth == "WPA2")     auth = "[WPA2_PSK]";
        else if (auth == "WPA3")     auth = "[WPA3_PSK]";
        else if (auth == "WPAWPA2")  auth = "[WPA_WPA2_PSK]";
        else if (auth == "WPA2EAP")  auth = "[WPA2_ENTERPRISE]";
        else if (auth == "WPA2WPA3") auth = "[WPA2_WPA3_PSK]";
        else                         auth = "[UNDEFINED]";
      }
      int    ch    = WiFi.channel(i);
      int    rssi  = WiFi.RSSI(i);
      String line  = bssid + "," + ssid + "," + auth + ","
                   + String(ch) + "," + String(rssi) + ",W";
      jcmkSendText(line);
      jcmkSentCount++;
    }
  }
  WiFi.scanDelete();
  nodeScanActive = false;
  pig_sweep_report(&nodeSweep, sweepYield);
}

// ================================================================
//  Lifecycle
// ================================================================
void enterNodeMode() {
  Serial.println("[MESH] Entering node mode");
  biscuitReporterReset();  // release ESP-NOW ownership from the solo reporter
  bleScannerShutdown();    // free the BT controller so ESP-NOW has clean 2.4 GHz airtime
  meshNodeActive        = false;
  jcmkHaveCore          = false;
  jcmkCoreFoundPending  = false;
  jcmkNetworksFound     = 0;
  jcmkSentCount         = 0;
  jcmkHbCounter         = 0;
  jcmkLastHbMs          = 0;
  jcmkLastReqMs         = 0;
  jcmkReqInterval       = JCMK_REQ_INIT_MS;
  jcmkStartIdx          = 0;
  jcmkEndIdx            = JCMK_NUM_CHANNELS - 1;
  jcmkAssignVer         = 0;
  nodeScanActive        = false;
  nodeScanAdminWin      = false;
  pig_sweep_invalidate(&nodeSweep);
  biscuitPlainReplyPending = false;
  biscuitChannelCount   = 0;

  // Mesh mode owns the WiFi stack — prevent stopAPIfAllowed() from firing
  // WiFi.disconnect(true,true) after esp_now_init() would kill the ESP-Now driver.
  apWindowActive = false;

  // Full WiFi deinit → reinit: clears previous AP channel/state from driver.
  // WiFi.disconnect(wifioff=true) only calls esp_wifi_stop() — it leaves the
  // driver in a tainted state that breaks ESP-Now receive on the next start.
  // WiFi.mode(WIFI_OFF) calls esp_wifi_deinit() for a true clean slate.
  WiFi.softAPdisconnect(true);
  WiFi.setAutoReconnect(false);
  WiFi.persistent(false);
  WiFi.mode(WIFI_OFF);  // full deinit — clears previous AP channel state
  delay(200);
  WiFi.mode(WIFI_STA);  // clean reinit
  delay(200);

  // Init ESP-Now FIRST, then lock the home channel.
  // Calling setChannel before esp_now_init() risks the driver
  // resetting the channel back during its own initialisation.
  esp_err_t err = esp_now_init();
  if (err != ESP_OK) {
    Serial.printf("[MESH] esp_now_init failed: %d\n", (int)err);
    return;
  }
  esp_now_register_recv_cb(jcmkOnRecv);

  // Lock radio to JCMK ESP-Now home channel AFTER init (matches JCMK pattern)
  delay(50);
  jcmkSetChannel(JCMK_ESPNOW_CH);
  // Verify the channel actually stuck
  { uint8_t pri; wifi_second_chan_t sec; esp_wifi_get_channel(&pri, &sec);
    Serial.printf("[MESH] Channel after set: %d (target=%d)%s\n", pri, JCMK_ESPNOW_CH,
      (pri == JCMK_ESPNOW_CH) ? " OK" : " *** MISMATCH — trying again");
    if (pri != JCMK_ESPNOW_CH) { delay(50); jcmkSetChannel(JCMK_ESPNOW_CH);
      esp_wifi_get_channel(&pri, &sec);
      Serial.printf("[MESH] Channel after retry: %d\n", pri); }
  }

  jcmkAddPeer(JCMK_BCAST);

  // Random stagger before first scan (JCMK begin() pattern: delay(random(100,5000))).
  // Prevents multiple nodes starting cycles in sync and flooding simultaneously.
  delay(random(200, 3000));

  meshNodeActive = true;
  Serial.println("[MESH] ESP-Now ready — searching for Core on ch 6...");
}

void exitNodeMode() {
  Serial.println("[MESH] Exiting node mode");
  if (meshNodeActive) esp_now_deinit();
  meshNodeActive = false;
  jcmkHaveCore   = false;

  // Full WiFi stack reset: OFF then STA gives a clean state after esp_now_deinit
  WiFi.mode(WIFI_OFF);
  delay(150);
  WiFi.setAutoReconnect(true);  // restore auto-reconnect for normal wardriving
  WiFi.mode(WIFI_STA);
  delay(100);

  // Drain GPS serial buffer accumulated during mesh mode scans
  while (GPSSerial.available()) gps.encode(GPSSerial.read());

  // Re-enable scanning so normal wardriving resumes immediately
  scanningEnabled = true;
}

// ================================================================
//  Core mode lifecycle
// ================================================================
void enterCoreMode() {
  Serial.println("[CORE] Entering Core mode");
  biscuitReporterReset();  // release ESP-NOW ownership from the solo reporter
  bleScannerShutdown();    // free the BT controller so ESP-NOW has clean 2.4 GHz airtime

  meshCoreActive = false;
  coreRecordsRx  = 0;
  coreNodeCount  = 0;
  coreAssignVer  = 0;
  coreHbCounter  = 0;
  coreLastHbMs   = 0;
  coreReqHead = coreReqTail = 0;
  coreTextHead = coreTextTail = 0;
  memset(coreNodes, 0, sizeof(coreNodes));
  meshProfilesInit();  // load saved sub-cluster profiles from SD

  // Mesh mode owns the WiFi stack — prevent stopAPIfAllowed() from firing
  // WiFi.disconnect(true,true) after esp_now_init() would kill the ESP-Now driver.
  apWindowActive = false;

  // Full WiFi deinit → reinit: clears previous AP channel/state from driver.
  WiFi.softAPdisconnect(true);
  WiFi.setAutoReconnect(false);
  WiFi.persistent(false);
  WiFi.mode(WIFI_OFF);  // full deinit — clears previous AP channel state
  delay(200);
  WiFi.mode(WIFI_STA);  // clean reinit
  delay(200);

  esp_err_t err = esp_now_init();
  if (err != ESP_OK) {
    Serial.printf("[CORE] esp_now_init failed: %d\n", (int)err);
    return;
  }
  esp_now_register_recv_cb(jcmkOnRecv);
  delay(50);
  jcmkSetChannel(JCMK_ESPNOW_CH);
  // Verify the channel actually stuck
  { uint8_t pri; wifi_second_chan_t sec; esp_wifi_get_channel(&pri, &sec);
    Serial.printf("[CORE] Channel after set: %d (target=%d)%s\n", pri, JCMK_ESPNOW_CH,
      (pri == JCMK_ESPNOW_CH) ? " OK" : " *** MISMATCH — trying again");
    if (pri != JCMK_ESPNOW_CH) { delay(50); jcmkSetChannel(JCMK_ESPNOW_CH);
      esp_wifi_get_channel(&pri, &sec);
      Serial.printf("[CORE] Channel after retry: %d\n", pri); }
  }
  jcmkAddPeer(JCMK_BCAST);

  meshCoreActive = true;

  // Bring BLE back up (it was shut down above for clean ESP-NOW airtime) so the
  // Biscuit Manager app sees this unit as a Biscuit Pro named "WarHog-Core...".
  // BLE + ESP-NOW coexist under the radio arbiter, same as the Arduino bridge;
  // expect a small ESP-NOW throughput cost while a phone is connected.
  // Latch the BLE personality for this Core session from the CORE BLE APP
  // setting, then stand up the matching standalone bridge.
  coreBleIsIosEsp = (cfg.blePersonality == "wardrivego");
  if (coreBleIsIosEsp) {
    // Advertises as "iosESP-XXXX" to the RocketGod Wardrive Go / WarBLE Go app.
    iosEspCompatBeginStandalone();
  } else {
    // Keep the Biscuit-facing GAP name SHORT ("WarHog"): it must fit the 31-byte
    // advertisement alongside the 128-bit Biscuit service UUID, or NimBLE drops
    // the local name and the app shows "Unknown".
    biscuitCompatBeginStandalone("WarHog");
  }

  Serial.printf("[CORE] Ready — listening for nodes on ch 6 (BLE app: %s)\n",
                coreBleIsIosEsp ? "Wardrive Go" : "Biscuit Manager");
#if PIG_FEAT_CORE_ISO
  coreStoreStart();   // Phase 4 Step 1: Core-mode SD logging runs on core 0, parallel to the loop
  coreRxDrainStart(); // Phase 4 Step 5: ESP-NOW record drain gets its own core-0 task
#endif
}

void exitCoreMode() {
  Serial.println("[CORE] Exiting Core mode");
  // Tear down whichever BLE bridge this session brought up, before WiFi/ESP-NOW churn.
  if (coreBleIsIosEsp) iosEspCompatStopStandalone();
  else                 biscuitCompatStopStandalone();
#if PIG_FEAT_CORE_ISO
  coreRxDrainStop();   // stop the core-0 record-drain task first (it feeds the SD queue)
  coreStoreStop();     // then stop the core-0 SD task before WiFi/ESP-NOW churn
#endif
  if (meshCoreActive) esp_now_deinit();
  meshCoreActive = false;
  coreNodeCount  = 0;

  WiFi.mode(WIFI_OFF);
  delay(150);
  WiFi.setAutoReconnect(true);  // restore auto-reconnect for normal wardriving
  WiFi.mode(WIFI_STA);
  delay(100);

  while (GPSSerial.available()) gps.encode(GPSSerial.read());
  scanningEnabled = true;
}

// ================================================================
//  Biscuit Reporter (solo mode)
// ================================================================
static void biscuitOnRecv(const esp_now_recv_info_t* info,
                          const uint8_t* data, int len) {
  (void)info;
  if (len < 5) return;
  if (data[0] != 'E' || data[1] != 'N' || data[2] != 'O' || data[3] != 'W') return;
  if (data[4] == JCMK_MSG_CORE_REPLY) biscuitLinked = true;
}

// ================================================================
//  Solo-mode phone bridge (cfg.blePhoneSolo)
//
//  In Core mode the phone bridge is mandatory — the app is the only window
//  into mesh traffic — so enterCoreMode() owns it. Solo wardriving used to
//  leave BLE idle, which meant the app saw nothing from a device that was not
//  in a mesh. This brings the SAME bridge up while solo and feeds it from the
//  local scanner, so one app works for both roles.
//
//  Radio ownership: the bridge only advertises + serves GATT, which coexists
//  with a BLE scan, but NimBLEDevice::init/deinit is shared with the
//  wardriving scanner and the Marauder detectors. So the teardown only frees
//  the controller when no other consumer is using it (see freeStack).
// ================================================================
static bool soloBridgeUp     = false;
static bool soloBridgeIosEsp = false;

bool phoneBridgeSoloActive() { return soloBridgeUp; }

// True while some other module owns the shared NimBLE stack.
static bool bleOwnedElsewhere() {
  return bleScannerActive() || bleDetectorsActive() ||
         appleBleSpamActive() || bleSpamActive();
}

static void soloBridgeStop() {
  if (!soloBridgeUp) return;
  soloBridgeUp = false;
  // Only hand the BT controller back if nothing else is on it.
  const bool freeStack = !bleOwnedElsewhere();
  if (soloBridgeIosEsp) iosEspCompatStopStandalone(freeStack);
  else                  biscuitCompatStopStandalone(freeStack);
  Serial.printf("[SOLO-BLE] Phone bridge down (stack %s)\n",
                freeStack ? "freed" : "left up for BLE scan/detectors");
}

void phoneBridgeSoloTick() {
  // A mesh role owns the bridge (Core) or the radio outright (Node).
  if (meshCoreActive || meshNodeActive) { soloBridgeStop(); return; }

  if (!cfg.blePhoneSolo) { soloBridgeStop(); return; }

  if (!soloBridgeUp) {
    // Latch the personality for this solo session, same selection Core makes.
    soloBridgeIosEsp = (cfg.blePersonality == "wardrivego");
    if (soloBridgeIosEsp) iosEspCompatBeginStandalone();
    else                  biscuitCompatBeginStandalone("WarHog");
    soloBridgeUp = true;
    Serial.printf("[SOLO-BLE] Phone bridge up (app: %s)\n",
                  soloBridgeIosEsp ? "Wardrive Go" : "Biscuit Manager");
  }

  // Service app commands + telemetry, and push GPS ~1 Hz so the app can pin
  // the mirrored APs to our position.
  if (soloBridgeIosEsp) {
    iosEspCompatSetGpsPresent(gps.charsProcessed() > 0);   // see coreModeTick
    iosEspCompatTick();
  } else {
    biscuitCompatTick();
  }

  static uint32_t soloGpsMs = 0;
  const uint32_t nowg = millis();
  if (gpsHasFix && nowg - soloGpsMs >= 1000) {
    soloGpsMs = nowg;
    if (soloBridgeIosEsp)
      iosEspCompatPushGps(gps.location.lat(), gps.location.lng(),
                          gps.altitude.meters(), (int)gps.satellites.value());
    else
      biscuitCompatPushGps(gps.location.lat(), gps.location.lng(),
                           gps.altitude.meters(), gps.hdop.hdop());
  }
}

void phoneBridgeSoloPushAp(const char* bssid, const char* ssid,
                           const char* auth, int ch, int rssi) {
  if (!soloBridgeUp) return;
  if (soloBridgeIosEsp) iosEspCompatPushAp(bssid, ssid, ch, rssi, auth);
  else                  biscuitCompatPushAp(bssid, ssid, ch, rssi, auth);
}

void biscuitReporterReset() {
  // Core/Node mode is taking the ESP-NOW driver (its WiFi.mode(OFF) deinits it),
  // so just drop our flags — we re-init cleanly next time we're solo.
  biscuitInited = false;
  biscuitReporterActive = false;
  biscuitLinked = false;
  biscuitFwdCount = 0;
}

void biscuitForwardNetwork(const String& bssid, const String& ssid,
                           const String& auth, int ch, int rssi) {
  if (!cfg.biscuitReport || meshCoreActive || meshNodeActive) return;
  if (biscuitFwdCount >= BISCUIT_FWD_QUEUE) return;  // drop overflow; device re-advertises
  String line = bssid + "," + ssid + "," + auth + "," + String(ch) + "," + String(rssi) + ",W";
  uint16_t slen = (line.length() < JCMK_TEXT_MAX) ? line.length() : JCMK_TEXT_MAX;
  memcpy(biscuitFwd[biscuitFwdCount], line.c_str(), slen);
  biscuitFwd[biscuitFwdCount][slen] = '\0';
  biscuitFwdCount++;
}

// Safe to retune to ch 6 only when solo, scanning (STA, not connected), no AP up.
static bool biscuitRadioSafe() {
  if (!cfg.biscuitReport || meshCoreActive || meshNodeActive) return false;
  if (apWindowActive) return false;
  if (WiFi.status() == WL_CONNECTED) return false;  // don't break a home-WiFi link
  const wifi_mode_t m = WiFi.getMode();
  return (m == WIFI_STA || m == WIFI_AP_STA);
}

void biscuitReporterFlush() {
  if (!biscuitRadioSafe()) {
    biscuitFwdCount = 0;  // can't send safely — discard this batch
    if (!cfg.biscuitReport && biscuitInited) {
      esp_now_deinit();
      biscuitInited = false;
      biscuitReporterActive = false;
      Serial.println("[BISCUIT] Reporter stopped");
    }
    return;
  }

  if (!biscuitInited) {
    if (esp_now_init() != ESP_OK) { biscuitFwdCount = 0; return; }
    esp_now_register_recv_cb(biscuitOnRecv);
    jcmkAddPeer(JCMK_BCAST);
    biscuitInited = true;
    biscuitReporterActive = true;
    biscuitLinked = false;
    biscuitLastReqMs = 0;  // announce immediately
    Serial.println("[BISCUIT] Reporter online (ch 6)");
  }

  jcmkSetChannel(JCMK_ESPNOW_CH);
  while (GPSSerial.available()) gps.encode(GPSSerial.read());

  const uint32_t now = millis();

  // Announce (CORE_REQUEST) until a manager replies, then only occasionally.
  const uint32_t reqGap = biscuitLinked ? 15000UL : 2000UL;
  if (now - biscuitLastReqMs >= reqGap) { jcmkSendCoreRequest(); biscuitLastReqMs = now; }

  // Keepalive heartbeat.
  if (now - biscuitLastHbMs >= JCMK_HB_MS) { jcmkSendHeartbeat(); biscuitLastHbMs = now; }

  // Forward this batch's networks as biscuit records.
  for (int i = 0; i < biscuitFwdCount; i++) {
    jcmkSendText(String(biscuitFwd[i]));
    biscuitRecordsSent++;
  }
  biscuitFwdCount = 0;
}

void coreModeTick() {
  if (!meshCoreActive) return;
#if PIG_FEAT_CORE_ISO
  // Measure this tick's distance from the last — the RX-drain cadence (DESIGN §13).
  { uint32_t us = micros();
    if (gLoopLastUs) {
      uint32_t gap = us - gLoopLastUs;
      if (gap > gLoopMaxUs) gLoopMaxUs = gap;
      if (gap > 5000)       gLoopGt5++;
    }
    gLoopLastUs = us; }
#endif
  // Service whichever phone-app bridge this session brought up.
  if (coreBleIsIosEsp) {
    // WD:CAPS is sent once, when the phone subscribes, so tell the bridge
    // whether a GPS module is WIRED (chars arriving) rather than letting it
    // infer presence from the first fix — otherwise a phone that connects
    // before lock is told gps=0 for the whole session.
    iosEspCompatSetGpsPresent(gps.charsProcessed() > 0);
    iosEspCompatTick();                      // Wardrive Go: commands + WD: telemetry
  } else {
    biscuitCompatTick();                     // Biscuit Manager: commands + telemetry
  }
  uint32_t now = millis();
  coreOtaTick();   // no-op unless a node firmware update is running

  // Stream the device position ~1 Hz so the app can pin APs to our location.
  static uint32_t biscuitGpsMs = 0;
  if (gpsHasFix && now - biscuitGpsMs >= 1000) {
    biscuitGpsMs = now;
    if (coreBleIsIosEsp)
      iosEspCompatPushGps(gps.location.lat(), gps.location.lng(),
                          gps.altitude.meters(), (int)gps.satellites.value());
    else
      biscuitCompatPushGps(gps.location.lat(), gps.location.lng(),
                           gps.altitude.meters(), gps.hdop.hdop());
  }

  // Node-roster snapshot is Biscuit-only (Wardrive Go has no node roster concept).
  // Stream it to the Biscuit app's Node Management screen — on request
  // (CMD:getnodesnapshot) or every ~3 s so it stays live. Capture the active node
  // indices first so the header count always matches the records.
  static uint32_t biscuitSnapMs = 0;
  if (!coreBleIsIosEsp && biscuitCompatActive() &&
      (biscuitCompatConsumeSnapshotRequest() || now - biscuitSnapMs >= 3000)) {
    biscuitSnapMs = now;
    uint8_t  act[CORE_MAX_NODES];
    uint16_t n = 0;
    for (uint8_t i = 0; i < CORE_MAX_NODES; i++)
      if (coreNodes[i].active) act[n++] = i;
    uint16_t sid = biscuitCompatSnapshotBegin(n);
    for (uint16_t k = 0; k < n; k++) {
      CoreNodeInfo& nd = coreNodes[act[k]];
      char mac[18];
      snprintf(mac, sizeof(mac), "%02X:%02X:%02X:%02X:%02X:%02X",
               nd.mac[0], nd.mac[1], nd.mac[2], nd.mac[3], nd.mac[4], nd.mac[5]);
      biscuitCompatSnapshotRecord(sid, k, mac, (int)nd.recordsRx, -50, 1 /*C5/biscuit*/);
    }
    biscuitCompatSnapshotComplete(sid);
  }

#if PIG_FEAT_DYNAMIC_ASSIGN
  coreYieldUpdate(now);   // Phase 3: fold per-channel record counts into yield EWMA
#endif

  // 1. Process pending CORE_REQUEST queue (new node registrations).
  // Drain the whole queue first, then reassign AT MOST ONCE — and only if the
  // roster actually changed. Reassigning per-request (the old behavior) ran the
  // blocking delay(10)-per-node assignment many times per tick, which stalled the
  // loop and made the Core screen lag badly whenever nodes were active/churning.
  static uint32_t coreLastReassignMs = 0;
  static bool     coreReassignPending = false;
  while (coreReqHead != coreReqTail) {
    uint8_t i = coreReqHead;
    coreReqHead = (coreReqHead + 1) % CORE_REQ_QUEUE;
    uint8_t before = coreNodeCount;
    coreFindOrAddNode(coreReqBuf[i].mac, coreReqBuf[i].isBiscuit, coreReqBuf[i].band24);
    if (coreNodeCount != before) coreReassignPending = true;  // a NEW node joined
  }
  // Coalesce bursts + rate-limit so reassignment can't thrash the loop.
  if (coreReassignPending && now - coreLastReassignMs >= 800) {
    coreReassignChannels();
    coreLastReassignMs  = now;
    coreReassignPending = false;
  }

  // 2. Process pending TEXT records
#if PIG_FEAT_CORE_ISO
  coreMirrorDrain();   // the core-0 tRxDrain task owns coreTextBuf now; here (core 1) we
                       // only drain the parsed-AP mirror ring for phone push + telemetry
#else
  while (coreTextHead != coreTextTail) {
    uint8_t i = coreTextHead;
    coreTextHead = (coreTextHead + 1) % CORE_TEXT_QUEUE;
#if PIG_FEAT_PAIR_SCHED
    coreParseAndLogText(coreTextBuf[i].line, coreTextBuf[i].srcNode);
#else
    coreParseAndLogText(coreTextBuf[i].line);
#endif
  }
#endif

#if PIG_FEAT_CORE_ISO
  // Phase 4 Step 1 instrumentation: watch the core-0 SD queue depth + drops.
  // heap/heap_min = free / lowest-ever-free INTERNAL RAM — the binding constraint
  // as the roster grows (scripts/mesh_signoff.py tracks it toward 12 nodes).
  static uint32_t coreIsoLogMs = 0;
  if (now - coreIsoLogMs >= 5000) {
    coreIsoLogMs = now;
    Serial.printf("[CORE-ISO] SD store q: depth=%u drops=%lu | rx_drop txt=%lu req=%lu txtq_max=%u"
                  " | rt_us max=%lu gt5ms=%lu | loop_us max=%lu gt5ms=%lu | nodes=%u"
                  " | heap=%u heap_min=%u\n",
                  gLogQ ? (unsigned)uxQueueMessagesWaiting(gLogQ) : 0u,
                  (unsigned long)gStoreDrops,
                  (unsigned long)gRxTextDrops, (unsigned long)gRxReqDrops, (unsigned)gRxQMax,
                  (unsigned long)gRtMaxUs, (unsigned long)gRtGt5,
                  (unsigned long)gLoopMaxUs, (unsigned long)gLoopGt5,
                  (unsigned)coreNodeCount,
                  (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
                  (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    // per-window reset so each line is self-contained
    gLoopMaxUs = 0; gLoopGt5 = 0; gRtMaxUs = 0; gRtGt5 = 0; gRxQMax = 0;
  }
#endif

#if PIG_FEAT_PAIR_SCHED
  // Phase 5: per-antenna-pair redundancy. pct = how often a group's second node saw a
  // BSSID its partner just saw (lower is better — the goal is complementary coverage).
  if (now - gPairLogMs >= 5000) {
    gPairLogMs = now;
    for (uint8_t g = 1; g <= CORE_MAX_GROUPS; g++) {
      if (gPairEither[g] == 0) continue;
      uint32_t pct = (gPairBoth[g] * 100u) / gPairEither[g];
      Serial.printf("[PAIR] grp=%u overlap both=%lu either=%lu pct=%lu%%\n",
                    (unsigned)g, (unsigned long)gPairBoth[g],
                    (unsigned long)gPairEither[g], (unsigned long)pct);
      gPairBoth[g] = 0; gPairEither[g] = 0;
    }
  }
#endif

  // 3. Periodic heartbeat + ADMIN refresh to all connected nodes.
  // Nodes that missed the ADMIN while scanning will recover within one cycle.
  if (now - coreLastHbMs >= CORE_HB_MS) {
    coreLastHbMs = now;
    coreSendHeartbeatToAll();
    // coreResendAdminToAll() blocks ~10 ms per node; skip it while an update is
    // streaming so the transfer isn't starved (nodes keep their last assignment).
    if (!coreOtaBusy()) coreResendAdminToAll();
  }

  // 3b. If Biscuit Report is on, also announce ourselves to a parent Biscuit
  // Manager with a full-size CORE_REQUEST so it lists this Core as a biscuit.
  // Our own nodes ignore CORE_REQUEST, and any reply the Manager sends is ignored
  // by the Core branch — so this doesn't disturb our mesh.
  static uint32_t coreAnnounceMs = 0;
  if (cfg.biscuitReport && (now - coreAnnounceMs >= 3000)) {
    coreAnnounceMs = now;
    jcmkSendCoreRequest();
  }

  // 4. Node timeout check
  bool changed = false;
  for (uint8_t i = 0; i < CORE_MAX_NODES; i++) {
    // The node being updated goes quiet on its normal channels (it stops scanning and
    // heartbeating while it receives), so don't let the roster drop it mid-transfer.
    if (coreOtaBusy() && (int8_t)i == ota.node) continue;
    if (coreNodes[i].active && (now - coreNodes[i].lastHbMs) > CORE_NODE_TIMEOUT) {
      Serial.printf("[CORE] Node %d timed out\n", i);
      esp_now_del_peer(coreNodes[i].mac);
      memset(&coreNodes[i], 0, sizeof(CoreNodeInfo));
      coreNodeCount--;
      changed = true;
    }
  }
  if (changed) coreReassignChannels();
}

// ================================================================
//  Loop tick
// ================================================================
void nodeModeTick() {
  if (!meshNodeActive) return;
  uint32_t now = millis();

  // Consume pending core-found event from the ESP-Now callback
  if (jcmkCoreFoundPending) {
    jcmkCoreFoundPending = false;
    memcpy(jcmkCoreMac, jcmkCoreMacPending, 6);
    jcmkHaveCore    = true;
    jcmkReqInterval = JCMK_REQ_INIT_MS;
    jcmkAddPeer(jcmkCoreMac);
    if (biscuitPlainReplyPending) {
      // Identify ourselves to the Biscuit Core ("WarHog:<version>") and give
      // it a short ch-6 admin window to send its role/channel-config handshake.
      jcmkSetChannel(JCMK_ESPNOW_CH);
      biscuitSendControl(7, 0, "WarHog:" FIRMWARE_VERSION);
      nodeScanAdminWin = true;
      nodeScanAdminMs  = now;
    }
    Serial.printf("[MESH] Core: %02X:%02X:%02X:%02X:%02X:%02X%s\n",
      jcmkCoreMac[0], jcmkCoreMac[1], jcmkCoreMac[2],
      jcmkCoreMac[3], jcmkCoreMac[4], jcmkCoreMac[5],
      biscuitPlainReplyPending ? " (Biscuit plain-reply)" : "");
  }

  // CORE_REQUEST with backoff (only while radio is free)
  if (!jcmkHaveCore && !nodeScanActive && (now - jcmkLastReqMs >= jcmkReqInterval)) {
    jcmkLastReqMs  = now;
    jcmkSetChannel(JCMK_ESPNOW_CH);
    jcmkSendCoreRequest();
    jcmkReqInterval = (jcmkReqInterval * 2 > JCMK_REQ_MAX_MS)
                      ? JCMK_REQ_MAX_MS : jcmkReqInterval * 2;
  }

  // Heartbeat backup timer (cycle-end heartbeat is primary; fires if scan stalls)
  if (jcmkHaveCore && !nodeScanActive && (now - jcmkLastHbMs >= JCMK_HB_MS)) {
    jcmkLastHbMs = now;
    jcmkSetChannel(JCMK_ESPNOW_CH);
    jcmkSendHeartbeat();
  }

  // Per-channel async scan — runs continuously while connected to Core
  if (jcmkHaveCore) nodeDoScanTick();
}

// ================================================================
//  OLED page renderer (page 5) — handles both Node and Core mode
// ================================================================
void drawPageMeshNode() {
  // Rendering is owned by the Freenove TFT frontend.
}

