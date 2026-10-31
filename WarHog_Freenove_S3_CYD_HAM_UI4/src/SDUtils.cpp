#include "SDUtils.h"
#include "Globals.h"
#include "esp_heap_caps.h"   // heap_caps_malloc — batch buffer in PSRAM when present
#include "esp_attr.h"        // DMA_ATTR (SdReader bounce buffer)
#include "SpiBusLock.h"
#include "BoardPins.h"       // BoardPins::SD_CS (W550 SPI SD bus recovery)
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>

// ---- SD bus recovery ----
// Ported from the XIAO C5 Core (warhog_fork/SDUtils.cpp). After sustained heavy
// SD I/O the SPI bus can wedge, so SD.open()/SD.exists() fail even though the
// card is fine. sdBeginBestClock() mounts at the fastest stable clock on a
// descending ladder; sdTryRecover() unmounts + remounts one rung slower.
// W550 only (real SPI SD). On the Freenove SD_MMC build these are safe no-ops
// (sdTryRecover() returns false) so shared call sites behave exactly as before
// until an SD_MMC recovery variant lands.
#if defined(BOARD_W550)
extern SPIClass sdSpi;   // defined in main.cpp

static const uint32_t kLadder[] = {
  20000000, 16000000, 12000000, 8000000, 4000000, 1000000
};
static uint32_t sdRunHz    = 0;   // clock the card mounted at
static uint32_t sdDynCapHz = 0;   // lowered by sdTryRecover() when the bus proves flaky
static uint32_t sdLastRecoverMs = 0;

bool sdBeginBestClock(uint8_t csPin) {
  uint32_t capHz = 20000000;   // CYD config has no sdMaxSpiHz; cap at the board default
  if (sdDynCapHz && sdDynCapHz < capHz) capHz = sdDynCapHz;

  for (size_t i = 0; i < sizeof(kLadder) / sizeof(kLadder[0]); i++) {
    uint32_t hz = kLadder[i];
    if (hz > capHz) continue;
    if (SD.begin(csPin, sdSpi, hz)) {
      Serial.printf("[SD] Running at %lu Hz\n", (unsigned long)hz);
      sdRunHz = hz;
      return true;
    }
    SD.end();
  }
  return false;
}

bool sdTryRecover() {
  uint32_t now = millis();
  if (now - sdLastRecoverMs < 4000) return false;   // rate-limit: don't hammer a dead card
  sdLastRecoverMs = now;

  // A wedge means the mounted clock isn't reliable here: remount one rung
  // slower (never below 4 MHz). Lasts until reboot.
  for (size_t i = 0; i + 1 < sizeof(kLadder) / sizeof(kLadder[0]); i++) {
    if (sdRunHz && kLadder[i] <= sdRunHz) {
      if (kLadder[i + 1] >= 4000000) sdDynCapHz = kLadder[i + 1];
      break;
    }
  }
  Serial.printf("[SD] Attempting SD bus recovery (SD.end + reinit, cap %lu Hz)...\n",
                (unsigned long)sdDynCapHz);
  SD.end();
  delay(50);
  bool ok = sdBeginBestClock(BoardPins::SD_CS);
  sdOk = ok;
  Serial.printf("[SD] Recovery %s\n", ok ? "OK" : "FAILED");
  return ok;
}
#else
bool sdBeginBestClock(uint8_t /*csPin*/) { return false; }   // SD_MMC variant TODO
bool sdTryRecover() { return false; }
#endif

// ---- Path helpers ----

String pathBasename(const String& p) {
  int slash = p.lastIndexOf('/');
  if (slash < 0) return p;
  return p.substring(slash + 1);
}

String normalizeSdPath(const char* dir, const char* nameIn) {
  if (!dir || !nameIn) return "";

  String d(dir);
  String n(nameIn);

  d.trim();
  n.trim();

  if (d.length() == 0 || n.length() == 0) return "";

  // Ensure dir starts with "/"
  if (d[0] != '/') d = "/" + d;

  // Strip trailing "/" from dir
  while (d.endsWith("/")) d.remove(d.length() - 1);

  // Case A: name is already absolute: "/logs/foo.csv" or "/uploaded/foo.csv"
  if (n[0] == '/') {
    // If SD lib already gives full path, just return it
    return n;
  }

  // Case B: name is "logs/foo.csv" (no leading slash)
  // If it starts with the same directory name, convert to absolute.
  // Example: dir="/logs", name="logs/foo.csv" => "/logs/foo.csv"
  String dNoSlash = d;
  if (dNoSlash.startsWith("/")) dNoSlash = dNoSlash.substring(1); // "logs"

  if (n.startsWith(dNoSlash + "/")) {
    return "/" + n;  // make it absolute
  }

  // Case C: name is just "foo.csv"
  // Join dir + "/" + name
  return d + "/" + n;
}

bool isAllowedDataPath(const String& p) {
  return p.startsWith("/logs/") || p.startsWith("/uploaded/");
}

// ---- Move a file into an SD subfolder ----
// Shared by moveToUploaded()/moveToArchived(). destDir is an absolute folder
// such as "/uploaded" or "/archived" (no trailing slash).
static bool moveToFolder(const String& srcPath, const char* destDir) {
  if (!sdOk) return false;
  if (!SD.exists(srcPath)) {
    // A wedged bus can make a present file look missing — remount and recheck
    // once (no-op on the SD_MMC build, so behaviour there is unchanged).
    if (!sdTryRecover() || !SD.exists(srcPath)) {
      Serial.printf("[SD] moveToFolder: source missing: %s\n", srcPath.c_str());
      return false;
    }
  }

  // Ensure folder exists
  if (!SD.exists(destDir)) {
    Serial.printf("[SD] Creating %s ...\n", destDir);
    if (!SD.mkdir(destDir)) {
      Serial.printf("[SD] ERROR: SD.mkdir(%s) failed\n", destDir);
      return false;
    }
  }

  String dstPath = String(destDir) + "/" + pathBasename(srcPath);

  // If destination exists, remove it first (rename may fail otherwise)
  if (SD.exists(dstPath)) {
    Serial.print("[SD] Removing existing dst: ");
    Serial.println(dstPath);
    SD.remove(dstPath);
  }

  Serial.print("[SD] Moving ");
  Serial.print(srcPath);
  Serial.print(" -> ");
  Serial.println(dstPath);

  if (SD.rename(srcPath, dstPath)) {
    Serial.println("[SD] Move OK");
    return true;
  }

  Serial.println("[SD] ERROR: SD.rename failed");
  // Last resort: copy + delete (some SD libs are picky)
  SdReader in(srcPath);                // DMA-safe read (see SdReader)
  if (!in) { Serial.println("[SD] copy fallback: open src failed"); return false; }

  File out = SD.open(dstPath, FILE_WRITE);
  if (!out) { Serial.println("[SD] copy fallback: open dst failed"); in.close(); return false; }

  uint8_t buf[1024];
  while (true) {
    int n = in.read(buf, sizeof(buf));
    if (n <= 0) break;
    out.write(buf, n);
    delay(0);
  }
  out.flush();
  out.close();
  in.close();

  // Verify copy
  if (!SD.exists(dstPath)) {
    Serial.println("[SD] copy fallback: dst does not exist after write");
    return false;
  }

  if (!SD.remove(srcPath)) {
    Serial.println("[SD] copy fallback: WARNING failed to remove src after copy");
    // still consider it moved-ish, but warn
  }

  Serial.println("[SD] copy fallback: OK");
  return true;
}

// ---- Move to uploaded / archived ----
bool moveToUploaded(const String& srcPath) { return moveToFolder(srcPath, "/uploaded"); }

// Files the firmware would otherwise auto-delete (empty/header-only CSVs from the
// boot/upload cleanup) are moved here instead, so the device never silently
// removes a file of its own accord.
bool moveToArchived(const String& srcPath) { return moveToFolder(srcPath, "/archived"); }

// ---- Log file ----

// Hard size cap per CSV file: keep every log safely under the 30 MB upload
// limit so each file can always be uploaded. This is byte-based (not a
// row-count estimate) so the ceiling holds regardless of row width — long
// SSIDs or RCOIs can't push a file over. The ~1 MB of headroom below 30 MB
// absorbs the final row plus the next file's header. When the cap is reached,
// the active CSV is closed and a fresh one is opened with proper WiGLE headers.
static const uint32_t CSV_MAX_BYTES = 9u * 1024u * 1024u;  // < 10 MB: Wardrift /v1/ingest/signals caps raw uploads at 10 MiB (and 200k records); 9 MiB leaves margin for one batch of overshoot. Still fine for WiGLE/WDGWars.
static uint32_t       csvBytes      = 0;

// ---- Batched CSV writer ----------------------------------------------------
// Rows are accumulated in a RAM buffer and written to the SD card in a single
// batch instead of one SD write per network. Two triggers drain the buffer:
//   1. it reaches SD_BATCH_LINES rows, or
//   2. SD_BATCH_FLUSH_MS has elapsed since the first buffered row — driven by
//      sdLogBufferTick() from loop(), so a partial batch can't sit in RAM
//      indefinitely and be lost when the device is powered off at the end of
//      a run.
// Worst-case loss on an abrupt power-off is one partial batch: fewer than
// SD_BATCH_LINES rows, no older than SD_BATCH_FLUSH_MS.
//
// The buffer is a fixed char block allocated from PSRAM when the board has it
// (freeing ~25 KB of scarce internal RAM on the S3/W550), falling back to
// internal RAM on boards without PSRAM. Writing to SD from a PSRAM source is
// safe: FatFS copies into its own sector cache, so there is no DMA-from-PSRAM
// constraint (that only affects DMA reads).
static const uint16_t SD_BATCH_LINES    = 100;     // rows per SD write
static const uint32_t SD_BATCH_FLUSH_MS = 20000;   // 15–30 s safety window
static const size_t   SD_ROW_MAX        = 256;     // worst-case bytes per row (+CRLF headroom)
static const size_t   SD_BATCH_CAP      = (size_t)SD_BATCH_LINES * SD_ROW_MAX;  // ~25 KB
static char*    s_logBuf      = nullptr; // batch buffer (PSRAM if available)
static size_t   s_logBufLen   = 0;       // bytes currently used
static uint16_t s_logBufLines = 0;       // rows currently buffered
static uint32_t s_bufStartMs  = 0;       // millis() of the first buffered row

// Allocate the batch buffer once: PSRAM first, then internal RAM, then plain
// malloc as a last resort. Kept for the program's lifetime (reused across log
// rotations). Returns false if no allocation succeeded.
static bool ensureLogBuffer() {
  if (s_logBuf) return true;
  const char* where = "PSRAM";
  s_logBuf = (char*)heap_caps_malloc(SD_BATCH_CAP, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!s_logBuf) { where = "internal"; s_logBuf = (char*)heap_caps_malloc(SD_BATCH_CAP, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT); }
  if (!s_logBuf) { where = "internal"; s_logBuf = (char*)malloc(SD_BATCH_CAP); }
  if (s_logBuf) Serial.printf("[SD] CSV batch buffer: %u bytes (%s)\n", (unsigned)SD_BATCH_CAP, where);
  else          Serial.println("[SD] CSV batch buffer alloc FAILED — writing rows unbuffered");
  return s_logBuf != nullptr;
}

// Sanitise a user-provided device name for safe use in filenames.
// Keeps alphanumerics, hyphens, underscores; replaces spaces with _;
// strips everything else; truncates to 20 chars.
static String sanitiseDeviceName(const String& raw) {
  String s = raw;
  s.replace(" ", "_");
  for (int i = (int)s.length() - 1; i >= 0; i--) {
    char c = s[i];
    if (!isAlphaNumeric(c) && c != '_' && c != '-') s.remove(i, 1);
  }
  if (s.length() > 20) s = s.substring(0, 20);
  return s;
}

static String newCsvFilename() {
  if (!SD.exists("/logs")) SD.mkdir("/logs");

  // Build optional prefix:  "name_WarHog_"  or empty
  String prefix = "";
  if (cfg.deviceName.length() > 0) {
    String safe = sanitiseDeviceName(cfg.deviceName);
    if (safe.length() > 0) prefix = safe + "_WarHog_";
  }

  // Make collisions extremely unlikely: millis + esp_random
  for (int tries = 0; tries < 25; tries++) {
    uint32_t r = (uint32_t)esp_random();
    char buf[100];
    snprintf(buf, sizeof(buf), "/logs/%sWiGLE_%lu_%08lX.csv",
             prefix.c_str(), (unsigned long)millis(), (unsigned long)r);
    String p(buf);
    if (!SD.exists(p)) return p;
  }

  // last-resort fallback
  char buf2[100];
  snprintf(buf2, sizeof(buf2), "/logs/%sWiGLE_%lu.csv",
           prefix.c_str(), (unsigned long)millis());
  return String(buf2);
}

bool openLogFile() {
  if (!sdOk) return false;

  // Close any previous handle
  closeLogFile();

  // Pick a fresh filename FIRST
  currentCsvPath = newCsvFilename();

  Serial.print("[SD] Opening log file: ");
  Serial.println(currentCsvPath);

  logFile = SD.open(currentCsvPath, FILE_WRITE);
  if (!logFile && sdTryRecover()) {       // wedged bus: remount once, then retry
    logFile = SD.open(currentCsvPath, FILE_WRITE);
  }
  if (!logFile) {
    Serial.println("[SD] Failed to open log file for write");
    return false;
  }

  // Build device field: WarHog-{name} if set, otherwise WarHog-Wardriver
  String deviceField = "WarHog-Wardriver";
  if (cfg.deviceName.length() > 0) {
    String safe = sanitiseDeviceName(cfg.deviceName);
    if (safe.length() > 0) deviceField = "WarHog-" + safe;
  }

  // Derive board model from the ACTIVE pin-map name (set by detectPinsByChip /
  // pickPinsFromConfig at boot). This reflects the real chip even when
  // cfg.board="auto", avoiding the old fallback that always wrote "Xiao-ESP32S3".
  String pn = String(pins.name); pn.toUpperCase();
  String boardModel;
  if      (pn.indexOf("EXP") >= 0) boardModel = "Xiao-ESP32S3-Exp";
  else if (pn.indexOf("C5")  >= 0) boardModel = "Xiao-ESP32C5";
  else if (pn.indexOf("C6")  >= 0) boardModel = "Xiao-ESP32C6";
  else                              boardModel = "Freenove-FNK0104B-ESP32S3";

  // WiGLE WiFi 1.6 header
  logFile.print("WigleWifi-1.6,appRelease=");
  logFile.print(FIRMWARE_VERSION);
  logFile.print(",model="); logFile.print(boardModel);
  logFile.print(",release=1,device="); logFile.print(deviceField);
  logFile.print(",display=ILI9341-240x320-Touch,board="); logFile.print(boardModel);
  logFile.println(",brand=WarHog,star=Sol,body=3,subBody=0");
  logFile.println("MAC,SSID,AuthMode,FirstSeen,Channel,Frequency,RSSI,CurrentLatitude,CurrentLongitude,AltitudeMeters,AccuracyMeters,RCOIs,MfgrId,Type");
  logFile.flush();
  // Seed the byte counter with the header size so the cap accounts for it.
  csvBytes = (uint32_t)logFile.size();

  // Make sure the batch buffer exists (PSRAM-backed when available).
  ensureLogBuffer();

  Serial.println("[SD] Log file initialized with WiGLE headers");
  return true;
}

// Write the buffered rows to the open file in one batch, flush, and clear the
// buffer. Does NOT rotate the CSV (flushLogBuffer() handles that afterwards) so
// it is safe to call from closeLogFile() during shutdown/rotation without
// re-entering the rotation path.
static void drainLogBuffer() {
  if (s_logBufLines == 0) return;
  if (!sdOk || !logFile || !s_logBuf) { s_logBufLen = 0; s_logBufLines = 0; return; }

  // Reset the counters up front (not the bytes): the reopen path below
  // closes/reopens the log file, which calls back into this function, and an
  // empty count makes that call a no-op. The buffer bytes stay intact so the
  // retry below can re-send the same batch.
  size_t len = s_logBufLen;
  s_logBufLen = 0;
  s_logBufLines = 0;

  size_t written = logFile.write((const uint8_t*)s_logBuf, len);
  csvBytes += (uint32_t)written;
  logFile.flush();   // commit the batch to the card now, not on a later timer

  // Silent write failure — write() returns short. Reopen the log once; if that
  // also fails, mark SD unusable until next boot (matches the old per-line path).
  static uint8_t consecFails = 0;
  if (len > 0 && written < len) {
    consecFails++;
    Serial.printf("[SD] Batch write short (%u/%u bytes, %u consecutive)\n",
                  (unsigned)written, (unsigned)len, consecFails);
    if (consecFails >= 3) {
      Serial.println("[SD] Attempting log reopen...");
      closeLogFile();              // buffer count already 0 here — no recursion
      if (openLogFile()) {
        Serial.println("[SD] Reopen OK — retrying batch");
        logFile.write((const uint8_t*)s_logBuf, len);  // bytes still intact
        logFile.flush();
        consecFails = 0;
      } else {
        Serial.println("[SD] Reopen FAILED — SD marked unusable");
        sdOk = false;
      }
    }
  } else {
    consecFails = 0;
  }
}

// Drain the batch to the card, then rotate the CSV if it has reached the
// WDGoWars size cap so every file stays < 30 MB.
static void flushLogBuffer() {
  drainLogBuffer();
  if (sdOk && logFile && csvBytes >= CSV_MAX_BYTES) {
    Serial.println("[SD] CSV size cap reached, rotating log file");
    closeLogFile();
    openLogFile();
  }
}

// Force every buffered row to the card and flush the handle — so an external
// reader (the serial toolkit) sees a consistent on-SD copy of the active log.
void sdLogFlushNow() {
  flushLogBuffer();
  if (sdOk && logFile) logFile.flush();
}

void closeLogFile() {
  drainLogBuffer();   // persist any buffered rows before the handle goes away
  if (logFile) {
    Serial.println("[SD] Closing log file");
    logFile.flush();
    logFile.close();
  }
}

// Time-based safety flush. Called from loop(): if rows have been waiting in the
// buffer longer than SD_BATCH_FLUSH_MS, write them out even if the batch isn't
// full. This bounds how long any row can sit in RAM before reaching the card.
void sdLogBufferTick() {
  if (s_logBufLines == 0) return;
  if ((millis() - s_bufStartMs) >= SD_BATCH_FLUSH_MS) flushLogBuffer();
}

// Shared writer: appends one row to the RAM batch buffer, writing the whole
// batch to SD once it fills. Used by both the WiFi and BLE row builders so they
// behave identically.
static void writeLogLine(const String& line) {
  if (!sdOk || !logFile) return;

  // No batch buffer (allocation failed) — fall back to writing the row straight
  // to the file so logging still works, just without batching.
  if (!ensureLogBuffer()) {
    size_t w = logFile.println(line);
    csvBytes += (uint32_t)w;
    logFile.flush();
    return;
  }

  size_t len = line.length();
  if (len + 2 > SD_ROW_MAX) len = SD_ROW_MAX - 2;   // clamp a pathologically long row

  // Flush first if this row (plus CRLF) wouldn't fit in the remaining space.
  if (s_logBufLen + len + 2 > SD_BATCH_CAP) flushLogBuffer();

  if (s_logBufLines == 0) s_bufStartMs = millis();  // start this batch's timer
  memcpy(s_logBuf + s_logBufLen, line.c_str(), len);
  s_logBufLen += len;
  s_logBuf[s_logBufLen++] = '\r';
  s_logBuf[s_logBufLen++] = '\n';
  s_logBufLines++;

  if (s_logBufLines >= SD_BATCH_LINES) flushLogBuffer();
}

bool appendWigleRow(const String& mac, const String& ssid, const String& auth,
                    const String& firstSeen, int channel, int rssi,
                    double lat, double lon, double altM, double accM) {
  if (!sdOk || !logFile) return false;

  // Network whitelist: skip logging any SSID the user has explicitly listed
  // (e.g. their own home/work network). Exact, case-sensitive match against
  // up to 10 configured entries; empty slots are never matched, so a blank
  // SSID (hidden network) is never accidentally filtered by an unused slot.
  if (ssid.length() > 0) {
    for (uint8_t i = 0; i < 10; i++) {
      if (cfg.ssidWhitelist[i].length() > 0 && cfg.ssidWhitelist[i] == ssid) {
        return false;
      }
    }
  }

  String safeSsid = ssid;
  safeSsid.replace("\"", "\"\"");

  String line;
  line.reserve(256);
  line += mac; line += ",";
  line += "\""; line += safeSsid; line += "\",";
  line += auth; line += ",";
  line += firstSeen; line += ",";
  line += String(channel); line += ",";
  // Frequency in MHz derived from channel number (WiGLE 1.6 requirement)
  uint32_t freq = 0;
  if      (channel >= 1  && channel <= 13) freq = 2407u + (uint32_t)channel * 5;
  else if (channel == 14)                  freq = 2484u;
  else if (channel >= 32)                  freq = 5000u + (uint32_t)channel * 5;
  line += String(freq); line += ",";
  line += String(rssi); line += ",";
  line += String(lat, 6); line += ",";
  line += String(lon, 6); line += ",";
  line += String(altM, 1); line += ",";
  line += String(accM, 1); line += ",";
  line += ",,WIFI"; // RCOIs (empty), MfgrId (empty), Type

  writeLogLine(line);
  return true;
}

// BLE observation row. Matches the WigleWifi-1.6 BLE contract WDGoWars accepts:
// AuthMode "[BLE]", Channel/Frequency 0, Type "BLE".
void appendWigleBleRow(const String& mac, const String& name, int rssi,
                       const String& firstSeen,
                       double lat, double lon, double altM, double accM) {
  if (!sdOk || !logFile) return;

  String safeName = name;
  safeName.replace("\"", "\"\"");

  String line;
  line.reserve(160);
  line += mac; line += ",";
  line += "\""; line += safeName; line += "\",";
  line += "[BLE],";
  line += firstSeen; line += ",";
  line += "0,0,";                     // Channel, Frequency
  line += String(rssi); line += ",";
  line += String(lat, 6); line += ",";
  line += String(lon, 6); line += ",";
  line += String(altM, 1); line += ",";
  line += String(accM, 1); line += ",";
  line += ",,BLE";                    // RCOIs (empty), MfgrId (empty), Type

  writeLogLine(line);
}

// ---- SdReader: DMA-safe reads (see SDUtils.h) ----

static DMA_ATTR uint8_t s_sdBounce[2048] __attribute__((aligned(64)));   // 4 sectors

static SemaphoreHandle_t sdBounceMutex() {
  static SemaphoreHandle_t m = xSemaphoreCreateMutex();
  return m;
}

SdReader::SdReader(const char* path) {
  if (!sdOk || !path || !*path) return;
  String full = String(SD.mountpoint()) + (path[0] == '/' ? "" : "/") + path;
  spiBusLock();
  int fd = ::open(full.c_str(), O_RDONLY);
  struct stat st;
  if (fd >= 0 && (fstat(fd, &st) != 0 || S_ISDIR(st.st_mode))) { ::close(fd); fd = -1; }
  spiBusUnlock();
  if (fd < 0) return;
  _fd = fd;
  _size = (size_t)st.st_size;
}

void SdReader::close() {
  if (_fd < 0) return;
  spiBusLock();
  ::close(_fd);
  spiBusUnlock();
  _fd = -1;
  _raLen = _raPos = 0;
}

int SdReader::available() const {
  if (_fd < 0) return 0;
  return (int)((_size > _pos ? _size - _pos : 0) + (size_t)(_raLen - _raPos));
}

int SdReader::rawRead(uint8_t* dst, size_t n) {
  size_t got = 0;
  while (got < n) {
    size_t want = n - got;
    if (want > sizeof(s_sdBounce)) want = sizeof(s_sdBounce);
    xSemaphoreTake(sdBounceMutex(), portMAX_DELAY);
    spiBusLock();
    int r = ::read(_fd, s_sdBounce, want);
    if (r > 0) memcpy(dst + got, s_sdBounce, (size_t)r);
    spiBusUnlock();
    xSemaphoreGive(sdBounceMutex());
    if (r <= 0) break;
    got  += (size_t)r;
    _pos += (size_t)r;
    if ((size_t)r < want) break;   // end of file
  }
  return (int)got;
}

bool SdReader::fill() {
  _raPos = 0;
  int r = rawRead(_ra, sizeof(_ra));
  _raLen = r > 0 ? (uint16_t)r : 0;
  return _raLen > 0;
}

int SdReader::read(uint8_t* dst, size_t n) {
  if (_fd < 0 || !dst) return 0;
  size_t got = 0;
  if (_raPos < _raLen) {                          // drain read-ahead first
    size_t k = _raLen - _raPos;
    if (k > n) k = n;
    memcpy(dst, _ra + _raPos, k);
    _raPos += (uint16_t)k;
    got = k;
  }
  if (got < n) got += (size_t)rawRead(dst + got, n - got);
  return (int)got;
}

int SdReader::read() {
  if (_fd < 0) return -1;
  if (_raPos >= _raLen && !fill()) return -1;
  return _ra[_raPos++];
}

String SdReader::readStringUntil(char term) {
  String out;
  if (_fd < 0) return out;
  while (true) {
    if (_raPos >= _raLen && !fill()) break;
    const uint8_t* start = _ra + _raPos;
    size_t len = _raLen - _raPos;
    const uint8_t* hit = (const uint8_t*)memchr(start, term, len);
    if (hit) {
      out.concat((const char*)start, (unsigned)(hit - start));
      _raPos += (uint16_t)(hit - start + 1);
      break;
    }
    out.concat((const char*)start, (unsigned)len);
    _raPos = _raLen;
  }
  return out;
}
