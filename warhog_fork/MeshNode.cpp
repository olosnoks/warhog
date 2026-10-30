#include "MeshNode.h"
#include "Globals.h"
#include "Config.h"
#include "GPS.h"
#include "SDUtils.h"
#include "BLEBridge.h"
#include "IosEspCompat.h"
#include "PigWorld.h"
#include "PigMetricsHook.h"   // Phase-0 instrumentation (observe-only)
#include <esp_now.h>
#include "esp_wifi.h"

#include "pig_dedup.h"   // Scheduler Phase 1: shared worker dedup (flag PIG_FEAT_DEDUP, default OFF)
#include "pig_dwell.h"   // Scheduler Phase 2: adaptive dwell/revisit (flag PIG_FEAT_ADAPTIVE_DWELL, default OFF)
#include "pig_proto.h"   // PIG_MSG_ADMIN_EXT / pig_admin_ext_t (worker parse is unconditional)

// ================================================================
//  Scheduler Phase 4 (Step 1) — move per-record SD logging off the main loop
//  onto a core-1 FreeRTOS task, so the RT path (ESP-NOW RX ring drain + scan)
//  stays responsive while the SD card stalls. Flag-gated PIG_FEAT_CORE_ISO
//  (default OFF -> inline write, zero change). Uses a FreeRTOS queue (cross-core
//  safe, unlike a hand-rolled ring) that drops newest on overflow (lossy: better
//  to lose a record than block the loop). Core-mode only, so the task is the sole
//  SD writer while active -> no SD mutex needed on this board (OLED is I2C).
// ================================================================
#ifndef PIG_FEAT_CORE_ISO
#define PIG_FEAT_CORE_ISO 0
#endif
#if PIG_FEAT_CORE_ISO
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
struct PigLogRec {
  char   mac[20], ssid[34], auth[20], ts[28];
  int16_t ch; int8_t rssi; double lat, lon, alt, acc;
};
static QueueHandle_t gLogQ      = nullptr;
static TaskHandle_t  gStoreTask = nullptr;
static volatile bool gStoreRun  = false;
static volatile uint32_t gStoreDrops = 0;   // records dropped when the SD queue is full
static void tStore(void*) {
  PigLogRec r;
  while (gStoreRun) {
    if (xQueueReceive(gLogQ, &r, pdMS_TO_TICKS(20)) == pdTRUE)
      appendWigleRow(String(r.mac), String(r.ssid), String(r.auth), String(r.ts),
                     r.ch, r.rssi, r.lat, r.lon, r.alt, r.acc);
  }
  vTaskDelete(nullptr);
}
static void coreStoreStart() {
  if (gStoreTask) return;
  if (!gLogQ) gLogQ = xQueueCreate(32, sizeof(PigLogRec));
  gStoreRun = true;
  BaseType_t coreId = (portNUM_PROCESSORS > 1) ? 1 : 0;
  xTaskCreatePinnedToCore(tStore, "tStore", 4096, nullptr, 3, &gStoreTask, coreId);
  Serial.printf("[CORE-ISO] SD store task started (core %d)\n", (int)coreId);
}
static void coreStoreStop() {
  if (!gStoreTask) return;
  gStoreRun = false;   // task self-deletes on its next wake
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
//  Scheduler Phase 3 — dynamic yield assignment via PIG_MSG_ADMIN_EXT (flag-gated).
//  When PIG_FEAT_DYNAMIC_ASSIGN=1 the Core tracks per-channel yield from received
//  records and hands each confirmed WarHog node an EXPLICIT yield-ordered channel
//  list (pig_admin_ext_t) instead of a contiguous range — the non-contiguous
//  yield-packing DESIGN §4 calls for. Every node also gets the 2.4 anchor {1,6,11}
//  so 2.4 GHz-only hardware stays useful (it self-filters 5 GHz). Legacy JCMK/Biscuit
//  peers keep the contiguous-range ADMIN. OFF -> legacy even split, zero change.
// ================================================================
#ifndef PIG_FEAT_DYNAMIC_ASSIGN
#define PIG_FEAT_DYNAMIC_ASSIGN 0
#endif
#if PIG_FEAT_DYNAMIC_ASSIGN
#include "yield_sched.h"   // scheduler/ (via -I../scheduler)
static ChanStat  gChanStat[PIG_MAX_CH];
static uint16_t  gChanHits[PIG_MAX_CH];
static uint32_t  gYieldLastMs = 0;
static SchedCfg  gSchedCfg;
static int chIndexOf(uint8_t ch) {
  for (uint8_t i = 0; i < JCMK_NUM_CHANNELS; i++) if (JCMK_CHANNELS[i] == ch) return (int)i;
  return -1;
}
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

// ---- WarHog-to-WarHog identification + transmit-slot scheduling ----
// A short marker carried in the otherwise-always-empty text/len fields of the
// existing CORE_REQUEST/CORE_REPLY/HEARTBEAT messages. Neither real JCMK
// hardware nor Biscuit nodes look at this payload, so it's fully backward
// compatible; it only ever activates the new scheduling path when BOTH sides
// of a pairing are confirmed to be genuine WarHog devices.
static const char*   WARHOG_MARKER     = "WARHOG1";
static const uint8_t WARHOG_MARKER_LEN = 7;

// Deterministic transmit-slot width for WarHog-to-WarHog nodes. Each
// confirmed-WarHog node gets an exclusive [slotIndex*SLOT_MS, +SLOT_MS) window
// within a repeating cycle (cycle length = SLOT_MS * active-WarHog-node-count),
// so scan results are sent without colliding with other nodes' transmissions.
static const uint32_t JCMK_SLOT_MS = 200;

// If a Node hasn't heard anything from its Core in this long, assume it's gone
// and return to searching (mirrors Biscuit's own CORE_TIMEOUT_MS behavior).
static const uint32_t JCMK_CORE_TIMEOUT_MS = 30000;

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
uint32_t jcmkSendFailCount = 0;
// Mesh size reported by the Core in its heartbeat (total active nodes it sees).
// A node only ever talks to the Core, never to peers, so this is its ONLY view
// of the whole mesh — used to render the true herd count on the node's screen.
uint8_t  jcmkMeshHerd    = 0;

static uint32_t jcmkHbCounter   = 0;
// Separate, always-incrementing counter for TEXT (scan-result) messages.
// A single channel scan can yield several networks, and jcmkSendText() used
// to stamp every one of them with the *current* jcmkHbCounter value instead
// of its own -- meaning every TEXT message sent between two heartbeats
// carried an identical, stale counter. If a Core treats (sender MAC,
// counter) as a duplicate-suppression key (a common anti-retry pattern),
// it would process the first message with that counter (the heartbeat,
// which is what keeps a node showing as "connected") and silently drop
// every TEXT message sharing it as an apparent duplicate -- which looks
// exactly like a node staying registered/alive while reporting 0 networks.
static uint32_t jcmkTextCounter = 0;
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
static volatile bool  jcmkCoreFoundPending    = false;
static uint8_t        jcmkCoreMacPending[6]   = {0};
static volatile bool  jcmkCoreIsWarHogPending = false;
static volatile bool  biscuitPlainReplyPending = false;
static uint8_t biscuitChannels[JCMK_NUM_CHANNELS + 2];
static volatile uint8_t biscuitChannelCount = 0;

// WarHog-to-WarHog transmit-slot state (only meaningful when jcmkCoreIsWarHog)
static bool     jcmkCoreIsWarHog  = false;
static uint8_t  jcmkSlotIndex     = 0;
static uint8_t  jcmkSlotCount     = 1;
static uint32_t jcmkCycleEpochMs  = 0;
static uint32_t jcmkCoreLastSeenMs = 0;  // for JCMK_CORE_TIMEOUT_MS detection

// Ring buffer of scan-result lines awaiting this node's transmit slot.
// Scanning (nodeDoScanTick) keeps running at full pace regardless of buffer
// state; only sending already-found results is deferred to the slot window.
#define JCMK_PENDING_MAX 64
struct JcmkPendingLine { char text[96]; };
static JcmkPendingLine jcmkPendingBuf[JCMK_PENDING_MAX];
static uint8_t         jcmkPendingHead = 0, jcmkPendingTail = 0;

static bool jcmkPendingEmpty() { return jcmkPendingHead == jcmkPendingTail; }

static bool jcmkPendingPush(const String& line) {
  uint8_t next = (jcmkPendingTail + 1) % JCMK_PENDING_MAX;
  if (next == jcmkPendingHead) return false;  // full — drop rather than block scanning
  size_t n = line.length();
  if (n >= sizeof(jcmkPendingBuf[0].text)) n = sizeof(jcmkPendingBuf[0].text) - 1;
  memcpy(jcmkPendingBuf[jcmkPendingTail].text, line.c_str(), n);
  jcmkPendingBuf[jcmkPendingTail].text[n] = '\0';
  jcmkPendingTail = next;
  return true;
}

static bool jcmkPendingPop(String& out) {
  if (jcmkPendingEmpty()) return false;
  out = jcmkPendingBuf[jcmkPendingHead].text;
  jcmkPendingHead = (jcmkPendingHead + 1) % JCMK_PENDING_MAX;
  return true;
}

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
static uint8_t  coreWarHogSlotCount = 0;  // active WarHog-confirmed node count; recomputed in coreReassignChannels()

static const uint32_t CORE_HB_MS        = 5000;
static const uint32_t CORE_NODE_TIMEOUT = 90000;  // 90 s — generous for many-node ESP-Now collisions

// Ring buffers: ESP-Now callback → main loop
#define CORE_REQ_QUEUE     16
#define CORE_TEXT_QUEUE    64
#define CORE_TEXT_SLOT_LEN 96  // sized for burst from 12 nodes × ~15 networks each

struct CorReqSlot  { uint8_t mac[6]; bool isBiscuit; bool isWarHog; };
struct CorTextSlot { char    line[CORE_TEXT_SLOT_LEN]; };

static CorReqSlot          coreReqBuf[CORE_REQ_QUEUE];
static volatile uint8_t    coreReqHead = 0, coreReqTail = 0;

static CorTextSlot         coreTextBuf[CORE_TEXT_QUEUE];
static volatile uint8_t    coreTextHead = 0, coreTextTail = 0;

// ================================================================
//  Local helpers
// ================================================================
// True if a received jcmk_text_msg_t carries the WarHog identification marker.
// `len` is the raw ESP-Now payload length (as delivered to the recv callback).
static bool jcmkHasWarHogMarker(const void* data, int len) {
  if (len < (int)sizeof(jcmk_text_msg_t)) return false;
  const jcmk_text_msg_t* tm = (const jcmk_text_msg_t*)data;
  if (tm->len < WARHOG_MARKER_LEN) return false;
  return memcmp(tm->text, WARHOG_MARKER, WARHOG_MARKER_LEN) == 0;
}

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

// ESP-Now send-status callback -- the only way to learn whether esp_now_send()
// actually made it onto the air. esp_now_send() itself only reports whether the
// driver *accepted* the request, not whether the frame was transmitted; a
// counter that only tracks calls made (like jcmkSentCount) can look perfectly
// healthy locally while every frame is silently failing at the radio layer.
static void jcmkOnSent(const esp_now_send_info_t* txInfo, esp_now_send_status_t status) {
  if (status != ESP_NOW_SEND_SUCCESS) {
    jcmkSendFailCount++;
    // txInfo->des_addr tells us whether this failure was for a unicast (Core
    // MAC) or broadcast (FF:FF:FF:FF:FF:FF) send -- a FAIL status here means
    // the frame was accepted by the driver and put on the air, but no
    // link-layer ACK came back (out of range, interference, or the peer
    // wasn't actually listening on this channel at that instant).
    if (txInfo && txInfo->des_addr) {
      const uint8_t* d = txInfo->des_addr;
      Serial.printf("[MESH] Send FAILED (no ACK) to %02X:%02X:%02X:%02X:%02X:%02X\n",
        d[0], d[1], d[2], d[3], d[4], d[5]);
    }
  }
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

static void jcmkSendCoreRequest() {
  jcmk_text_msg_t msg = {};
  memcpy(msg.magic, JCMK_MAGIC, 4);
  msg.type    = JCMK_MSG_CORE_REQUEST;
  msg.counter = 0;
  // Marker identifies this as a genuine WarHog node to a WarHog Core; real
  // JCMK hardware and Biscuit Cores simply ignore this payload.
  msg.len     = WARHOG_MARKER_LEN;
  memcpy(msg.text, WARHOG_MARKER, WARHOG_MARKER_LEN);
  msg.text[WARHOG_MARKER_LEN] = '\0';
  esp_now_send(JCMK_BCAST, (uint8_t*)&msg, sizeof(msg));
}

static void jcmkSendHeartbeat() {
  jcmk_text_msg_t msg = {};
  memcpy(msg.magic, JCMK_MAGIC, 4);
  msg.type    = JCMK_MSG_HEARTBEAT;
  msg.counter = ++jcmkHbCounter;
  // Carry the marker here too so a Core can learn isWarHog even if it first
  // (re)discovers this node via a heartbeat rather than a fresh CORE_REQUEST.
  msg.len     = WARHOG_MARKER_LEN;
  memcpy(msg.text, WARHOG_MARKER, WARHOG_MARKER_LEN);
  msg.text[WARHOG_MARKER_LEN] = '\0';
  // Unicast only for a confirmed WarHog-to-WarHog pairing, where we control
  // peer registration on both ends and gain real link-layer retry/ACK.
  // Broadcast for everything else (real JCMK hardware / Biscuit): field
  // testing against real Biscuit hardware showed unicast frames getting a
  // 100% "no ACK" failure rate even though esp_now_send() itself always
  // accepted them -- i.e. the Biscuit's ESP-Now stack doesn't ack unicast
  // from a sender it hasn't itself registered as a peer. Broadcast frames
  // don't require the receiver to have the sender peered at all, which is
  // exactly why the discovery handshake (also broadcast) works fine.
  const uint8_t* dest = jcmkCoreIsWarHog ? jcmkCoreMac : JCMK_BCAST;
  esp_err_t err = esp_now_send(dest, (uint8_t*)&msg, sizeof(msg));
  if (err != ESP_OK) {
    // esp_now_send() itself rejected the call (peer not found, channel
    // mismatch, queue full, etc.) -- distinct from a FAIL reported later by
    // jcmkOnSent(), which means the frame *did* go out but got no ACK.
    uint8_t pri; wifi_second_chan_t sec; esp_wifi_get_channel(&pri, &sec);
    Serial.printf("[MESH] HEARTBEAT send() rejected: err=%d ch=%d dest=%02X:%02X:%02X:%02X:%02X:%02X\n",
      (int)err, pri, dest[0], dest[1], dest[2], dest[3], dest[4], dest[5]);
  }
}

static void jcmkSendText(const String& s) {
  jcmk_text_msg_t msg = {};
  memcpy(msg.magic, JCMK_MAGIC, 4);
  msg.type    = JCMK_MSG_TEXT;
  msg.counter = ++jcmkTextCounter;
  uint16_t slen = (uint16_t)((s.length() < JCMK_TEXT_MAX) ? s.length() : JCMK_TEXT_MAX);
  msg.len = slen;
  memcpy(msg.text, s.c_str(), slen);
  msg.text[slen] = '\0';
  // Always send full struct size — Biscuit Pro drops variable-length packets < 212 bytes.
  // Unicast only for confirmed WarHog-to-WarHog pairings (see jcmkSendHeartbeat
  // comment above for why broadcast is required for real JCMK/Biscuit hardware).
  const uint8_t* dest = jcmkCoreIsWarHog ? jcmkCoreMac : JCMK_BCAST;
  esp_err_t err = esp_now_send(dest, (uint8_t*)&msg, sizeof(msg));
#if PIG_FEAT_METRICS
  pigMetricsOnMeshTx(sizeof(msg));   // 212 B record frame onto the mesh (observe-only)
#endif
  if (err != ESP_OK) {
    uint8_t pri; wifi_second_chan_t sec; esp_wifi_get_channel(&pri, &sec);
    Serial.printf("[MESH] TEXT send() rejected: err=%d ch=%d dest=%02X:%02X:%02X:%02X:%02X:%02X\n",
      (int)err, pri, dest[0], dest[1], dest[2], dest[3], dest[4], dest[5]);
  }
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
  // Marker tells a WarHog node that this Core is also a genuine WarHog, so it
  // can safely switch to slot-scheduled transmit; ignored by third-party nodes.
  msg.len     = WARHOG_MARKER_LEN;
  memcpy(msg.text, WARHOG_MARKER, WARHOG_MARKER_LEN);
  msg.text[WARHOG_MARKER_LEN] = '\0';
  esp_now_send(mac, (uint8_t*)&msg, sizeof(msg));
}

// Send Biscuit Node a role assignment (MSG_ROLE_ASSIGN, type=5) then a channel
// config (MSG_CONFIG_UPDATE, type=10). Both packets must be full size (212 bytes).
// Called from coreReassignChannels / coreResendAdminToAll for isBiscuit nodes.
static void coreSendBiscuitRoleAndConfig(const uint8_t* mac, uint8_t startIdx, uint8_t endIdx) {
  // Step 1: role assignment — text[0] = 1 (ROLE_WIFI)
  // Biscuit MSG_ROLE_ASSIGN = type 5, same numeric value as JCMK_MSG_ADMIN.
  // After receiving this, Biscuit transitions from STATE_WAITING_ROLE to STATE_SCANNING.
  jcmk_text_msg_t roleMsg = {};
  memcpy(roleMsg.magic, JCMK_MAGIC, 4);
  roleMsg.type    = JCMK_MSG_ADMIN;  // = 5 = MSG_ROLE_ASSIGN on Biscuit
  roleMsg.counter = 0;
  roleMsg.len     = 1;
  roleMsg.text[0] = 1;  // ROLE_WIFI
  esp_now_send(mac, (uint8_t*)&roleMsg, sizeof(roleMsg));
  delay(10);

  // Step 2: channel config — "channels=1,2,...;dwell=80"
  // Biscuit MSG_CONFIG_UPDATE = type 10. Payload uses actual channel numbers
  // (not indices) in comma-separated list.
  jcmk_text_msg_t cfgMsg = {};
  memcpy(cfgMsg.magic, JCMK_MAGIC, 4);
  cfgMsg.type    = 10;  // MSG_CONFIG_UPDATE
  cfgMsg.counter = 0;

  String chList = "channels=";
  uint8_t end = (endIdx < JCMK_NUM_CHANNELS) ? endIdx : (JCMK_NUM_CHANNELS - 1);
  for (uint8_t j = startIdx; j <= end; j++) {
    if (j > startIdx) chList += ",";
    chList += String(JCMK_CHANNELS[j]);
  }
  chList += ";dwell=";
  chList += String((uint32_t)NODE_SCAN_DWELL_MS);

  uint16_t slen = (chList.length() < JCMK_TEXT_MAX)
                ? (uint16_t)chList.length() : (uint16_t)JCMK_TEXT_MAX;
  cfgMsg.len = slen;
  memcpy(cfgMsg.text, chList.c_str(), slen);
  cfgMsg.text[slen] = '\0';
  esp_now_send(mac, (uint8_t*)&cfgMsg, sizeof(cfgMsg));

  Serial.printf("[CORE] Biscuit role+config sent (ch %d-%d, dwell %dms)\n",
    JCMK_CHANNELS[startIdx],
    JCMK_CHANNELS[end],
    (int)NODE_SCAN_DWELL_MS);
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
        coreReqBuf[coreReqTail].isWarHog  = jcmkHasWarHogMarker(data, len);
        coreReqTail = next;
      }
    } else if (type == JCMK_MSG_TEXT && len >= 11) {
      const jcmk_text_msg_t* tm = (const jcmk_text_msg_t*)data;
      uint8_t next = (coreTextTail + 1) % CORE_TEXT_QUEUE;
      if (next != coreTextHead) {
        uint16_t slen = (tm->len < CORE_TEXT_SLOT_LEN - 1) ? tm->len : (CORE_TEXT_SLOT_LEN - 1);
        memcpy(coreTextBuf[coreTextTail].line, tm->text, slen);
        coreTextBuf[coreTextTail].line[slen] = '\0';
        coreTextTail = next;
      }
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
            // TEXT carries scan-line data, not the marker -- a node rediscovered
            // via TEXT (rare; HEARTBEAT/CORE_REQUEST arrive far more often) is
            // conservatively treated as non-WarHog until its next heartbeat.
            coreReqBuf[coreReqTail].isWarHog = false;
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
          coreReqBuf[coreReqTail].isWarHog  = jcmkHasWarHogMarker(data, len);
          coreReqTail = nxt;
        }
      }
    }
  } else {
    // ---- Node role (existing) ----
    if (type == JCMK_MSG_CORE_REPLY && !jcmkHaveCore && !jcmkCoreFoundPending) {
      memcpy(jcmkCoreMacPending, info->src_addr, 6);
      jcmkCoreIsWarHogPending = jcmkHasWarHogMarker(data, len);
      biscuitPlainReplyPending = biscuitHasPlainReply(data, len);
      jcmkCoreFoundPending = true;
    } else if (type == JCMK_MSG_HEARTBEAT && jcmkHaveCore
               && memcmp(info->src_addr, jcmkCoreMac, 6) == 0) {
      // Core's periodic broadcast: liveness signal for timeout detection, and
      // (for WarHog pairings) the shared timing anchor for slot scheduling.
      jcmkCoreLastSeenMs = millis();
      if (jcmkCoreIsWarHog) jcmkCycleEpochMs = millis();
      // WarHog extension: the Core stamps the mesh size (total active nodes)
      // behind the marker. This is a node's only view of the whole mesh, so
      // read it to render the true herd count on screen.
      {
        const jcmk_text_msg_t* hb = (const jcmk_text_msg_t*)data;
        if (len >= (int)sizeof(jcmk_text_msg_t)
            && hb->len >= WARHOG_MARKER_LEN + 1
            && memcmp(hb->text, WARHOG_MARKER, WARHOG_MARKER_LEN) == 0) {
          jcmkMeshHerd = (uint8_t)hb->text[WARHOG_MARKER_LEN];
        }
      }
    } else if (jcmkHaveCore && memcmp(info->src_addr, jcmkCoreMac, 6) == 0
               && len >= (int)sizeof(jcmk_text_msg_t) && type == JCMK_MSG_ADMIN) {
      // Plain-reply Biscuit admin handshake step: ack with type=15, echoing
      // back the first text byte (or low byte of counter as a fallback).
      const jcmk_text_msg_t* msg = (const jcmk_text_msg_t*)data;
      jcmkCoreLastSeenMs = millis();
      biscuitSendControl(15, msg->len ? (uint8_t)msg->text[0] : msg->counter & 0xFF);
    } else if (jcmkHaveCore && memcmp(info->src_addr, jcmkCoreMac, 6) == 0
               && len >= (int)sizeof(jcmk_text_msg_t) && type == 10) {
      // Plain-reply Biscuit channel-config message (MSG_CONFIG_UPDATE) --
      // parse the assigned channel list, then ack with type=11.
      jcmkCoreLastSeenMs = millis();
      if (biscuitPlainReplyPending) biscuitSetChannels((const jcmk_text_msg_t*)data);
      biscuitSendControl(11);
    } else if (type == PIG_MSG_ADMIN_EXT && len >= (int)pig_admin_ext_len(0)) {
      // Phase 3: explicit yield-packed channel list from a WarHog Core. Feed it into
      // the same biscuitChannels[] path the scan loop already uses for explicit lists.
      jcmkCoreLastSeenMs = millis();
      if (jcmkCoreIsWarHog) jcmkCycleEpochMs = millis();
      const pig_admin_ext_t* ax = (const pig_admin_ext_t*)data;
      if (ax->assign_ver != jcmkAssignVer) {
        jcmkAssignVer = ax->assign_ver;
        uint8_t avail = (uint8_t)(len - (int)pig_admin_ext_len(0));   // bytes available for channels
        uint8_t cnt   = ax->n;
        if (cnt > avail) cnt = avail;
        if (cnt > JCMK_NUM_CHANNELS) cnt = JCMK_NUM_CHANNELS;
        for (uint8_t i = 0; i < cnt; i++) biscuitChannels[i] = ax->channels[i];
        biscuitChannelCount = cnt;
        pig_sweep_invalidate(&nodeSweep);   // restart the scan cycle on the new list
      }
      if (jcmkCoreIsWarHog) {
        jcmkSlotIndex = ax->slot_index;
        jcmkSlotCount = (ax->slot_count > 0) ? ax->slot_count : 1;
      }
    } else if (type == JCMK_MSG_ADMIN && len >= (int)sizeof(jcmk_admin_msg_t)) {
      jcmkCoreLastSeenMs = millis();
      if (jcmkCoreIsWarHog) jcmkCycleEpochMs = millis();
      const jcmk_admin_msg_t* adm = (const jcmk_admin_msg_t*)data;
      if (adm->assignment_version != jcmkAssignVer) {
        jcmkAssignVer = adm->assignment_version;
        jcmkStartIdx  = adm->start_channel_idx;
        jcmkEndIdx    = adm->end_channel_idx;
        pig_sweep_invalidate(&nodeSweep);   // restart the scan cycle on the new range
      }
      // Transmit-slot fields (node_index/node_count) are only meaningful when
      // paired with a confirmed WarHog Core; refreshed every ADMIN regardless
      // of assignment_version since Core recomputes them on every send.
      if (jcmkCoreIsWarHog) {
        jcmkSlotIndex = adm->node_index;
        jcmkSlotCount = (adm->node_count > 0) ? adm->node_count : 1;
      }
    }
  }
}

// ================================================================
//  Core mode helpers (main loop only — not ISR-safe)
// ================================================================
static void coreFindOrAddNode(const uint8_t* mac, bool isBiscuit, bool isWarHog) {
  for (uint8_t i = 0; i < CORE_MAX_NODES; i++) {
    if (coreNodes[i].active && memcmp(coreNodes[i].mac, mac, 6) == 0) {
      coreNodes[i].lastHbMs = millis();
      // A node can go from "not yet confirmed WarHog" to confirmed once its
      // first HEARTBEAT/CORE_REQUEST with the marker arrives; never downgrade.
      if (isWarHog) coreNodes[i].isWarHog = true;
      return;  // already registered
    }
  }
  for (uint8_t i = 0; i < CORE_MAX_NODES; i++) {
    if (!coreNodes[i].active) {
      coreNodes[i].active    = true;
      coreNodes[i].lastHbMs  = millis();
      coreNodes[i].recordsRx = 0;
      coreNodes[i].isBiscuit = isBiscuit;
      coreNodes[i].isWarHog  = isWarHog;
      coreNodes[i].slotIndex = 0;  // assigned by coreReassignChannels() below
      memcpy(coreNodes[i].mac, mac, 6);
      coreNodeCount++;
      jcmkAddPeer(mac);
      // Re-send CORE_REPLY now that the peer is registered.
      // The initial reply from jcmkOnRecv() may have failed because the node's
      // MAC wasn't yet in the peer list when esp_now_send() was called.
      coreSendReply(mac);
      Serial.printf("[CORE] New %s node %d: %02X:%02X:%02X:%02X:%02X:%02X\n",
        isWarHog ? "WarHog" : (isBiscuit ? "Biscuit" : "JCMK"),
        i, mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
      return;
    }
  }
  Serial.println("[CORE] Max nodes reached");
}

static void coreReassignChannels() {
  // Build list of active slot indices
  uint8_t slots[CORE_MAX_NODES];
  uint8_t count = 0;
  for (uint8_t i = 0; i < CORE_MAX_NODES; i++)
    if (coreNodes[i].active) slots[count++] = i;
  if (count == 0) return;

  // Split JCMK_CHANNELS[] evenly; last node gets the remainder
  uint8_t perNode  = JCMK_NUM_CHANNELS / count;
  uint8_t startIdx = 0;
  for (uint8_t n = 0; n < count; n++) {
    coreNodes[slots[n]].startIdx = startIdx;
    coreNodes[slots[n]].endIdx   = (n < count - 1)
                                   ? (startIdx + perNode - 1)
                                   : (JCMK_NUM_CHANNELS - 1);
    startIdx += perNode;
  }
  coreAssignVer++;

  // Assign deterministic transmit-slot indices among WarHog-confirmed nodes
  // only -- JCMK/Biscuit nodes don't participate in slot scheduling and are
  // completely unaffected by this (their node_index/node_count keep the
  // legacy per-all-nodes numbering, which both already ignore anyway).
  uint8_t warhogIdx = 0;
  for (uint8_t n = 0; n < count; n++) {
    if (coreNodes[slots[n]].isWarHog) coreNodes[slots[n]].slotIndex = warhogIdx++;
  }
  coreWarHogSlotCount = warhogIdx;

  // Send assignment to each node using its protocol:
  // - Biscuit: MSG_ROLE_ASSIGN (type=5, text[0]=ROLE_WIFI) + MSG_CONFIG_UPDATE (type=10, channel string)
  // - JCMK:    jcmk_admin_msg_t (type=5, packed binary channel indices)
  for (uint8_t n = 0; n < count; n++) {
    uint8_t slot = slots[n];
#if PIG_FEAT_DYNAMIC_ASSIGN
    if (coreNodes[slot].isWarHog) {
      // Yield-packed explicit list (PIG_MSG_ADMIN_EXT): every node gets the 2.4
      // anchor {1,6,11} plus a round-robin slice of the yield-ordered 5 GHz set,
      // so productive channels are covered and 2.4-only nodes stay useful.
      uint8_t n5 = (uint8_t)(JCMK_NUM_CHANNELS - 14);
      uint8_t ord5[PIG_MAX_CH], schans[PIG_MAX_CH], rank[PIG_MAX_CH]; ChanStat sstat[PIG_MAX_CH];
      for (uint8_t j = 0; j < n5; j++) { schans[j] = JCMK_CHANNELS[14 + j]; sstat[j] = gChanStat[14 + j]; }
      sched_rank(sstat, schans, n5, millis(), gSchedCfg, rank);
      for (uint8_t j = 0; j < n5; j++) ord5[j] = schans[rank[j]];
      pig_admin_ext_t ax{};
      memcpy(ax.magic, JCMK_MAGIC, 4);
      ax.type        = PIG_MSG_ADMIN_EXT;
      ax.assign_ver  = coreAssignVer;
      ax.slot_index  = coreNodes[slot].slotIndex;
      ax.slot_count  = coreWarHogSlotCount;
      ax.scan_policy = PIG_SP_ACTIVE_ALLOWED | PIG_SP_PASSIVE_5G;
      uint8_t k = 0;
      ax.channels[k++] = 1; ax.channels[k++] = 6; ax.channels[k++] = 11;        // 2.4 anchor
      for (uint8_t j = n; j < n5 && k < PIG_ADMIN_EXT_MAXCH; j += count) ax.channels[k++] = ord5[j];
      ax.n = k;
      esp_now_send(coreNodes[slot].mac, (uint8_t*)&ax, pig_admin_ext_len(ax.n));
      delay(10);
      continue;
    }
#endif
    if (coreNodes[slot].isBiscuit) {
      coreSendBiscuitRoleAndConfig(coreNodes[slot].mac,
                                   coreNodes[slot].startIdx,
                                   coreNodes[slot].endIdx);
    } else {
      jcmk_admin_msg_t msg;
      memcpy(msg.magic, JCMK_MAGIC, 4);
      msg.type               = JCMK_MSG_ADMIN;
      msg.assignment_version = coreAssignVer;
      msg.start_channel_idx  = coreNodes[slot].startIdx;
      msg.end_channel_idx    = coreNodes[slot].endIdx;
      if (coreNodes[slot].isWarHog) {
        msg.node_index = coreNodes[slot].slotIndex;
        msg.node_count = coreWarHogSlotCount;
      } else {
        msg.node_index = n;      // legacy semantics for non-WarHog peers (unused by them)
        msg.node_count = count;
      }
      esp_now_send(coreNodes[slot].mac, (uint8_t*)&msg, sizeof(msg));
    }
    delay(10);
  }
  Serial.printf("[CORE] Reassigned channels: %d nodes v%d (%d WarHog slot%s)\n",
    count, coreAssignVer, coreWarHogSlotCount, coreWarHogSlotCount == 1 ? "" : "s");
}

// Re-send the current ADMIN assignment to every node.
// Called periodically so nodes that missed the update (e.g. mid-scan) recover.
static void coreResendAdminToAll() {
  if (coreNodeCount == 0) return;
  uint8_t n = 0;
  for (uint8_t i = 0; i < CORE_MAX_NODES; i++) {
    if (!coreNodes[i].active) continue;
    if (coreNodes[i].isBiscuit) {
      coreSendBiscuitRoleAndConfig(coreNodes[i].mac,
                                   coreNodes[i].startIdx,
                                   coreNodes[i].endIdx);
    } else {
      jcmk_admin_msg_t msg;
      memcpy(msg.magic, JCMK_MAGIC, 4);
      msg.type               = JCMK_MSG_ADMIN;
      msg.assignment_version = coreAssignVer;
      msg.start_channel_idx  = coreNodes[i].startIdx;
      msg.end_channel_idx    = coreNodes[i].endIdx;
      if (coreNodes[i].isWarHog) {
        msg.node_index = coreNodes[i].slotIndex;
        msg.node_count = coreWarHogSlotCount;
      } else {
        msg.node_index = n;
        msg.node_count = coreNodeCount;
      }
      esp_now_send(coreNodes[i].mac, (uint8_t*)&msg, sizeof(msg));
    }
    n++;
    delay(10);
  }
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
  // so every node can display the real herd count. The marker keeps this
  // invisible to stock JCMK/Biscuit nodes, which ignore heartbeat text.
  memcpy(msg.text, WARHOG_MARKER, WARHOG_MARKER_LEN);
  msg.text[WARHOG_MARKER_LEN] = (char)coreNodeCount;
  msg.len     = WARHOG_MARKER_LEN + 1;
  esp_now_send(JCMK_BCAST, (uint8_t*)&msg, sizeof(msg));
}

static void coreParseAndLogText(const char* line) {
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
#if PIG_FEAT_METRICS
  // Observe-only: a record received from a worker is one sighting (Core role).
  pigMetricsOnRecordStr(bssid.c_str(), rssi, ch, ssid.c_str(), auth.c_str());
#endif

  double lat = 0, lon = 0, altM = 0, accM = 0;
  if (gpsHasFix) {
    lat  = gps.location.lat();
    lon  = gps.location.lng();
    altM = gps.altitude.meters();
    accM = gps.hdop.hdop();
  }
  String ts = iso8601NowUTC();
#if PIG_FEAT_CORE_ISO
  // Phase 4: hand the SD write to the core-1 store task (non-blocking). Count on
  // enqueue; the task applies the same whitelist filter when it writes.
  coreStoreEnqueue(bssid, ssid, auth, ts, ch, rssi, lat, lon, altM, accM);
  coreRecordsRx++;
#else
  // Upstream: appendWigleRow() now returns false for whitelisted/failed rows,
  // which must not inflate the received-record count.
  if (appendWigleRow(bssid, ssid, auth, ts, ch, rssi, lat, lon, altM, accM)) {
    coreRecordsRx++;
  }
#endif

  // Mirror the same record to the BLE bridge (enqueue only; notified in tick).
  // The BLE feed carries every observed record regardless of CSV whitelisting.
  bleBridgePushRecord(bssid.c_str(), ssid.c_str(), auth.c_str(),
                      ch, rssi, lat, lon, altM, accM, ts.c_str());
}

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

    // Scan this single channel with the plan the sweep picked: a short active
    // fast pass over the whole assignment, then longer PASSIVE revisits of the
    // channels that actually yielded. DFS 5 GHz is always passive. With the
    // flag off every plan is the legacy fixed active scan at NODE_SCAN_DWELL_MS
    // and there is no revisit pass, so behavior is unchanged.
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

#if PIG_FEAT_METRICS
    // Observe-only: count every sighting + uniques for the §13 baseline line.
    // Runs before the role branch so it covers both legacy and WarHog paths.
    for (int mi = 0; mi < n; ++mi) {
      String ssidM = WiFi.SSID(mi);
      pigMetricsOnScanResult(WiFi.BSSID(mi), (int8_t)WiFi.RSSI(mi),
                             (uint8_t)WiFi.channel(mi), ssidM.c_str(),
                             (uint8_t)WiFi.encryptionType(mi));
    }
#endif

    double lat = 0, lon = 0, altM = 0, accM = 0;
    if (gpsHasFix) {
      lat  = gps.location.lat();
      lon  = gps.location.lng();
      altM = gps.altitude.meters();
      accM = gps.hdop.hdop();
    }
    String ts = iso8601NowUTC();

    if (!jcmkCoreIsWarHog) {
      // Legacy immediate-send path: non-WarHog Core (real JCMK hardware or
      // Biscuit Pro), or Core type not yet confirmed. Unchanged from before.
      if (jcmkHaveCore) jcmkSetChannel(JCMK_ESPNOW_CH);
      for (int i = 0; i < n; i++) {
        if ((i & 0x07) == 7 && pigWorldActive()) pigWorldTick();
        if (pigDedupSkip(i)) { pigMetricsOnSuppressed(); continue; }  // Phase 1 dedup (no-op unless flag)
        sweepYield++;                                                 // Phase 2 revisit ranking
        String bssid = WiFi.BSSIDstr(i);
        String ssid  = WiFi.SSID(i);
        String auth  = meshAuthModeToString(WiFi.encryptionType(i));
        int    ch    = WiFi.channel(i);
        int    rssi  = WiFi.RSSI(i);

        // Mirror every node-discovered AP to the BLE phone bridge (Wardrive Go / Biscuit).
        bleBridgePushRecord(bssid.c_str(), ssid.c_str(), auth.c_str(),
                            ch, rssi, lat, lon, altM, accM, ts.c_str());

        if (jcmkHaveCore) {
          if (biscuitPlainReplyPending) {
            if (auth == "OPEN") auth = "[OPEN]";
            else if (auth == "WEP") auth = "[WEP]";
            else if (auth == "WPA") auth = "[WPA_PSK]";
            else if (auth == "WPA2") auth = "[WPA2_PSK]";
            else if (auth == "WPA3") auth = "[WPA3_PSK]";
            else if (auth == "WPAWPA2") auth = "[WPA_WPA2_PSK]";
            else if (auth == "WPA2EAP") auth = "[WPA2_ENTERPRISE]";
            else if (auth == "WPA2WPA3") auth = "[WPA2_WPA3_PSK]";
            else auth = "[UNDEFINED]";
          }
          String line  = bssid + "," + ssid + "," + auth + ","
                       + String(ch) + "," + String(rssi) + ",W";
          // Log the exact payload content -- we've verified scanning finds
          // networks and sends are attempted/delivered at the radio level, but
          // never actually inspected what's inside the message being sent.
          Serial.printf("[MESH] TX: %s\n", line.c_str());
          jcmkSendText(line);
          jcmkSentCount++;
        }
      }
    } else {
      // WarHog-to-WarHog: buffer results instead of sending immediately.
      // Scanning keeps running at full pace, untouched by other nodes'
      // schedules -- jcmkWarHogSlotTick() flushes this buffer only during
      // this node's own deterministic transmit slot.
      for (int i = 0; i < n; i++) {
        if ((i & 0x07) == 7 && pigWorldActive()) pigWorldTick();
        if (pigDedupSkip(i)) { pigMetricsOnSuppressed(); continue; }  // Phase 1 dedup (no-op unless flag)
        sweepYield++;                                                 // Phase 2 revisit ranking
        String bssid = WiFi.BSSIDstr(i);
        String ssid  = WiFi.SSID(i);
        String auth  = meshAuthModeToString(WiFi.encryptionType(i));
        int    ch    = WiFi.channel(i);
        int    rssi  = WiFi.RSSI(i);

        // Mirror every node-discovered AP to the BLE phone bridge (Wardrive Go / Biscuit).
        bleBridgePushRecord(bssid.c_str(), ssid.c_str(), auth.c_str(),
                            ch, rssi, lat, lon, altM, accM, ts.c_str());

        if (jcmkHaveCore) {
          String line  = bssid + "," + ssid + "," + auth + ","
                       + String(ch) + "," + String(rssi) + ",W";
          jcmkPendingPush(line);
        }
      }
    }
  }
  WiFi.scanDelete();
  nodeScanActive = false;
  pig_sweep_report(&nodeSweep, sweepYield);
}

// ================================================================
//  WarHog-to-WarHog deterministic transmit-slot flush (see MeshNode plan).
//  Only ever active when jcmkCoreIsWarHog is true; otherwise a no-op, so
//  non-WarHog pairings are completely unaffected. Never interrupts an
//  in-flight scan (only runs in the natural gap between channel scans),
//  so a node's own scanning loop is never slowed down by this.
// ================================================================
static void jcmkWarHogSlotTick() {
  if (!jcmkCoreIsWarHog) return;
  if (nodeScanActive) return;      // never interrupt a live async scan
  if (jcmkPendingEmpty()) return;

  uint32_t slotCount = (jcmkSlotCount > 0) ? jcmkSlotCount : 1;
  uint32_t cycleMs   = JCMK_SLOT_MS * slotCount;
  uint32_t cyclePos  = (millis() - jcmkCycleEpochMs) % cycleMs;
  uint32_t slotStart = (uint32_t)jcmkSlotIndex * JCMK_SLOT_MS;

  if (cyclePos < slotStart || cyclePos >= slotStart + JCMK_SLOT_MS) return;  // not my turn yet

  jcmkSetChannel(JCMK_ESPNOW_CH);
  String line;
  uint8_t sent = 0;
  while (sent < 16 && jcmkPendingPop(line)) {
    jcmkSendText(line);
    jcmkSentCount++;
    sent++;
  }
}

// ================================================================
//  Lifecycle
// ================================================================
void enterNodeMode() {
  Serial.println("[MESH] Entering node mode");
  meshNodeActive        = false;
  jcmkHaveCore          = false;
  jcmkMeshHerd          = 0;
  jcmkCoreFoundPending  = false;
  jcmkCoreIsWarHogPending = false;
  jcmkCoreIsWarHog      = false;
  jcmkSlotIndex         = 0;
  jcmkSlotCount         = 1;
  jcmkCycleEpochMs      = 0;
  jcmkCoreLastSeenMs    = 0;
  jcmkPendingHead       = 0;
  jcmkPendingTail       = 0;
  jcmkNetworksFound     = 0;
  jcmkSentCount         = 0;
  jcmkSendFailCount     = 0;
  jcmkHbCounter         = 0;
  jcmkTextCounter       = 0;
  jcmkLastHbMs          = 0;
  jcmkLastReqMs         = 0;
  jcmkReqInterval       = JCMK_REQ_INIT_MS;
  jcmkStartIdx          = 0;
  jcmkEndIdx            = JCMK_NUM_CHANNELS - 1;
  jcmkAssignVer         = 0;
  nodeScanActive        = false;
  nodeScanAdminWin      = false;
  pig_sweep_invalidate(&nodeSweep);
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
  esp_now_register_send_cb(jcmkOnSent);

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

  // Random stagger before first scan so multiple nodes don't start cycles in sync.
  // Keep PigWorld rendering during the wait so the OLED doesn't freeze.
  uint32_t staggerEnd = millis() + (uint32_t)random(150, 750);
  while ((int32_t)(millis() - staggerEnd) < 0) {
    if (pigWorldActive()) pigWorldTick();
    delay(10);
  }

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

  meshCoreActive = false;
  coreRecordsRx  = 0;
  coreNodeCount  = 0;
  coreAssignVer  = 0;
  coreHbCounter  = 0;
  coreLastHbMs   = 0;
  coreReqHead = coreReqTail = 0;
  coreTextHead = coreTextTail = 0;
  memset(coreNodes, 0, sizeof(coreNodes));

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
  esp_now_register_send_cb(jcmkOnSent);
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

  // Start the BLE bridge so an iOS/iPad app can see live mesh data. BLE runs
  // under WiFi/BLE coexistence alongside the active ESP-Now mesh.
  bleBridgeBegin();

#if PIG_FEAT_CORE_ISO
  coreStoreStart();   // Phase 4: SD logging runs on core 1 while Core mode is active
#endif
  Serial.println("[CORE] Ready — listening for nodes on ch 6");
}

void exitCoreMode() {
  Serial.println("[CORE] Exiting Core mode");
#if PIG_FEAT_CORE_ISO
  coreStoreStop();
#endif
  // Keep BLE up across Core -> Node / Solo transitions when blePhoneSolo is
  // enabled so the Wardrive Go connection stays unbroken across mode switches.
  if (!cfg.blePhoneSolo) {
    bleBridgeStop();
  }
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

void coreModeTick() {
  if (!meshCoreActive) return;
  uint32_t now = millis();
#if PIG_FEAT_DYNAMIC_ASSIGN
  coreYieldUpdate(now);   // Phase 3: fold per-channel record counts into yield EWMA
#endif
#if PIG_FEAT_CORE_ISO
  { static uint32_t isoLogMs = 0;
    if (now - isoLogMs >= 5000) { isoLogMs = now;
      Serial.printf("[CORE-ISO] SD store q: depth=%u drops=%lu\n",
        gLogQ ? (unsigned)uxQueueMessagesWaiting(gLogQ) : 0u, (unsigned long)gStoreDrops); } }
#endif

  // 1. Process pending CORE_REQUEST queue (new node registrations)
  while (coreReqHead != coreReqTail) {
    uint8_t i = coreReqHead;
    coreReqHead = (coreReqHead + 1) % CORE_REQ_QUEUE;
    coreFindOrAddNode(coreReqBuf[i].mac, coreReqBuf[i].isBiscuit, coreReqBuf[i].isWarHog);
    coreReassignChannels();
  }

  // 2. Process pending TEXT records
  while (coreTextHead != coreTextTail) {
    uint8_t i = coreTextHead;
    coreTextHead = (coreTextHead + 1) % CORE_TEXT_QUEUE;
    coreParseAndLogText(coreTextBuf[i].line);
  }

  // 3. Periodic heartbeat + ADMIN refresh to all connected nodes.
  // Nodes that missed the ADMIN while scanning will recover within one cycle.
  if (now - coreLastHbMs >= CORE_HB_MS) {
    coreLastHbMs = now;
    coreSendHeartbeatToAll();
    coreResendAdminToAll();
  }

  // 4. Node timeout check
  bool changed = false;
  for (uint8_t i = 0; i < CORE_MAX_NODES; i++) {
    if (coreNodes[i].active && (now - coreNodes[i].lastHbMs) > CORE_NODE_TIMEOUT) {
      Serial.printf("[CORE] Node %d timed out\n", i);
      esp_now_del_peer(coreNodes[i].mac);
      memset(&coreNodes[i], 0, sizeof(CoreNodeInfo));
      coreNodeCount--;
      changed = true;
    }
  }
  if (changed) coreReassignChannels();

  // 5. If no mesh nodes are linked yet but a phone app (Wardrive Go) is
  //    actively streaming, run a lightweight async scan on JCMK_ESPNOW_CH (ch 6)
  //    so the radio never leaves the admin channel while still feeding live APs
  //    to the phone app in lone-Core mode.
  {
    static bool     coreSelfScanActive = false;
    static uint32_t coreSelfScanLastMs = 0;
    if (coreNodeCount == 0 && iosEspCompatStreaming()) {
      if (!coreSelfScanActive && (now - coreSelfScanLastMs >= 3000)) {
        coreSelfScanLastMs = now;
        int16_t rc = WiFi.scanNetworks(/*async*/true, /*hidden*/true,
                                        /*passive*/false, 120, JCMK_ESPNOW_CH);
        if (rc == WIFI_SCAN_RUNNING || rc == 0) coreSelfScanActive = true;
      } else if (coreSelfScanActive) {
        int n = WiFi.scanComplete();
        if (n >= 0) {
          for (int i = 0; i < n; i++) {
            String line = WiFi.BSSIDstr(i) + "," + WiFi.SSID(i) + ","
                        + meshAuthModeToString(WiFi.encryptionType(i)) + ","
                        + String(WiFi.channel(i)) + "," + String(WiFi.RSSI(i)) + ",W";
            coreParseAndLogText(line.c_str());
          }
          WiFi.scanDelete();
          coreSelfScanActive = false;
          jcmkSetChannel(JCMK_ESPNOW_CH);
        } else if (n < 0 && n != WIFI_SCAN_RUNNING) {
          WiFi.scanDelete();
          coreSelfScanActive = false;
        }
      }
    } else if (coreSelfScanActive) {
      if (WiFi.scanComplete() != WIFI_SCAN_RUNNING) {
        WiFi.scanDelete();
        coreSelfScanActive = false;
        jcmkSetChannel(JCMK_ESPNOW_CH);
      }
    }
  }

  // 6. Service the BLE bridge: drain record queue, refresh roster/status,
  //    apply any control commands from the app.
  bleBridgeTick();
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
    jcmkHaveCore       = true;
    jcmkCoreIsWarHog   = jcmkCoreIsWarHogPending;
    jcmkCoreLastSeenMs = now;
    jcmkCycleEpochMs   = now;
    jcmkReqInterval    = JCMK_REQ_INIT_MS;
    jcmkAddPeer(jcmkCoreMac);
    if (biscuitPlainReplyPending) {
      // Identify ourselves to the Biscuit Core ("WarHog:<version>") and give
      // it a short ch-6 admin window to send its role/channel-config handshake.
      jcmkSetChannel(JCMK_ESPNOW_CH);
      biscuitSendControl(7, 0, "WarHog:" FIRMWARE_VERSION);
      nodeScanAdminWin = true;
      nodeScanAdminMs = now;
    }
    Serial.printf("[MESH] Core: %02X:%02X:%02X:%02X:%02X:%02X%s\n",
      jcmkCoreMac[0], jcmkCoreMac[1], jcmkCoreMac[2],
      jcmkCoreMac[3], jcmkCoreMac[4], jcmkCoreMac[5],
      jcmkCoreIsWarHog ? " (WarHog — slot scheduling active)" : "");
  }

  // Core-timeout detection: if the Core goes silent, stop scanning/sending
  // into the void and return to actively searching for a (possibly new) Core.
  if (jcmkHaveCore && (now - jcmkCoreLastSeenMs >= JCMK_CORE_TIMEOUT_MS)) {
    Serial.println("[MESH] Core timed out — returning to search");
    if (esp_now_is_peer_exist(jcmkCoreMac)) esp_now_del_peer(jcmkCoreMac);
    jcmkHaveCore     = false;
    jcmkCoreIsWarHog = false;
    jcmkAssignVer    = 0;
    jcmkMeshHerd     = 0;
    jcmkStartIdx     = 0;
    jcmkEndIdx       = JCMK_NUM_CHANNELS - 1;
    pig_sweep_invalidate(&nodeSweep);   // the assignment is gone — rebuild the sweep
    jcmkSlotIndex    = 0;
    jcmkSlotCount    = 1;
    jcmkPendingHead  = jcmkPendingTail = 0;  // drop any buffered-but-unsent results
    nodeScanActive   = false;
    nodeScanAdminWin = false;
    biscuitChannelCount = 0;
    jcmkReqInterval  = JCMK_REQ_INIT_MS;
  }

  // CORE_REQUEST with backoff (only while radio is free)
  if (!jcmkHaveCore && !nodeScanActive && (now - jcmkLastReqMs >= jcmkReqInterval)) {
    jcmkLastReqMs  = now;
    jcmkSetChannel(JCMK_ESPNOW_CH);
    jcmkSendCoreRequest();
    if (iosEspCompatStreaming()) {
      // Hold on ch 6 for the admin window so CORE_REPLY can arrive before
      // resuming channel scans for the connected phone app.
      nodeScanAdminWin = true;
      nodeScanAdminMs  = now;
    }
    jcmkReqInterval = (jcmkReqInterval * 2 > JCMK_REQ_MAX_MS)
                      ? JCMK_REQ_MAX_MS : jcmkReqInterval * 2;
  }

  // Heartbeat backup timer (cycle-end heartbeat is primary; fires if scan stalls)
  if (jcmkHaveCore && !nodeScanActive && (now - jcmkLastHbMs >= JCMK_HB_MS)) {
    jcmkLastHbMs = now;
    jcmkSetChannel(JCMK_ESPNOW_CH);
    jcmkSendHeartbeat();
  }

  // Per-channel async scan — runs continuously while connected to Core, OR
  // whenever Wardrive Go is actively streaming from this Node.
  if (jcmkHaveCore || iosEspCompatStreaming()) {
    jcmkWarHogSlotTick();
    nodeDoScanTick();
  }
}

// ================================================================
//  OLED page renderer (page 5) — handles both Node and Core mode
// ================================================================
void drawPageMeshNode() {
  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);
  display.setTextSize(2);
  display.setCursor(0, 0);

  if (meshCoreActive) {
    // ---- Core mode ----
    display.print("CORE");
    // Node count in small text next to header
    display.setTextSize(1);
    char nb[5]; snprintf(nb, sizeof(nb), "[%d]", coreNodeCount);
    display.setCursor(52, 5);
    display.print(nb);
    display.drawFastHLine(0, 15, 128, SSD1306_WHITE);

    display.setTextSize(1);

    // Row 1 (y=17): status
    display.setCursor(0, 17);
    if (coreNodeCount == 0) {
      display.print("Waiting for nodes");
    } else {
      char buf[22];
      snprintf(buf, sizeof(buf), "%d node%s connected",
               coreNodeCount, coreNodeCount == 1 ? "" : "s");
      display.print(buf);
    }

    // Rows 2-3 (y=26, y=35): first 2 active node slots
    uint8_t shown = 0;
    for (uint8_t i = 0; i < CORE_MAX_NODES && shown < 2; i++) {
      int y = (shown == 0) ? 26 : 35;
      display.setCursor(0, y);
      if (coreNodes[i].active) {
        uint8_t* m = coreNodes[i].mac;
        uint8_t si = coreNodes[i].startIdx, ei = coreNodes[i].endIdx;
        char buf[22];
        snprintf(buf, sizeof(buf), "%02X:%02X:%02X %d-%d",
                 m[3], m[4], m[5],
                 (si < JCMK_NUM_CHANNELS) ? JCMK_CHANNELS[si] : 0,
                 (ei < JCMK_NUM_CHANNELS) ? JCMK_CHANNELS[ei] : 0);
        display.print(buf);
        shown++;
      }
    }
    if (shown == 0) {
      display.setCursor(0, 26); display.print("--:--:-- ------");
      display.setCursor(0, 35); display.print("--:--:-- ------");
    } else if (shown == 1) {
      display.setCursor(0, 35); display.print("--:--:-- ------");
    }

    // Row 4 (y=44): total records received
    display.setCursor(0, 44);
    char rbuf[22];
    snprintf(rbuf, sizeof(rbuf), "Rcvd: %lu", (unsigned long)coreRecordsRx);
    display.print(rbuf);

    // Row 5 (y=53): GPS + hint
    display.setCursor(0, 53);
    display.print(gpsHasFix ? "GPS:FIX" : "GPS:---");
    display.setCursor(62, 53);
    display.print("Hold=Node");

  } else {
    // ---- Node mode (original layout) ----
    display.print("Mesh");
    display.drawFastHLine(0, 15, 128, SSD1306_WHITE);
    display.setTextSize(1);

    // Row 1 (y=17): link status
    display.setCursor(0, 17);
    if (!meshNodeActive) {
      display.print("Init error");
    } else if (!jcmkHaveCore) {
      display.print("Searching...");
    } else {
      display.print("Core linked");
    }

    // Row 2 (y=26): core MAC
    display.setCursor(0, 26);
    if (jcmkHaveCore) {
      char macStr[18];
      snprintf(macStr, sizeof(macStr), "%02X:%02X:%02X:%02X:%02X:%02X",
        jcmkCoreMac[0], jcmkCoreMac[1], jcmkCoreMac[2],
        jcmkCoreMac[3], jcmkCoreMac[4], jcmkCoreMac[5]);
      display.print(macStr);
    } else {
      display.print("--:--:--:--:--:--");
    }

    // Row 3 (y=35): channel assignment + ENOW ch
    display.setCursor(0, 35);
    display.print("Ch:");
    if (jcmkAssignVer > 0 && jcmkStartIdx < JCMK_NUM_CHANNELS
                          && jcmkEndIdx   < JCMK_NUM_CHANNELS) {
      display.print(JCMK_CHANNELS[jcmkStartIdx]);
      display.print("-");
      display.print(JCMK_CHANNELS[jcmkEndIdx]);
    } else {
      display.print("all");
    }
    display.setCursor(72, 35);
    display.print("ENOW:");
    display.print(JCMK_ESPNOW_CH);

    // Row 4 (y=44): networks found + Core hint
    display.setCursor(0, 44);
    display.print("Found:");
    display.print(jcmkNetworksFound);
    display.setCursor(70, 44);
    display.print("Hold=Core");

    // Row 5 (y=53): records sent + radio-level send failures.
    // "Sent" only means esp_now_send() was called; "Fail" reflects the
    // actual delivery-status callback, so a healthy Sent count with a
    // climbing Fail count means the radio itself is dropping the frames.
    display.setCursor(0, 53);
    display.print("Sent:");
    display.print(jcmkSentCount);
    display.setCursor(70, 53);
    display.print("Fail:");
    display.print(jcmkSendFailCount);
  }

  display.display();
}
