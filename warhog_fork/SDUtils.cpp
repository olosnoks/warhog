#include "SDUtils.h"
#include "Globals.h"
#include "esp_heap_caps.h"   // heap_caps_malloc — batch buffer in PSRAM when present

// Path/name helpers (pathBasename, normalizeSdPath, isAllowedDataPath,
// sanitiseDeviceName) now live in common/sd_paths.h, included via SDUtils.h.

// ---- SD clock negotiation ----
// Tries a descending list of SPI clock speeds (fastest first, capped by
// cfg.sdMaxSpiHz) and returns as soon as one mounts successfully, so the
// card runs as fast as the wiring/card actually supports instead of being
// pinned to a conservative fixed speed.
static const uint32_t kLadder[] = {
  20000000, 16000000, 12000000, 8000000, 4000000, 1000000
};
static uint32_t sdRunHz    = 0;   // clock the card mounted at
static uint32_t sdDynCapHz = 0;   // lowered by sdTryRecover() when the bus proves flaky (0 = none)

bool sdBeginBestClock(uint8_t csPin) {
  uint32_t capHz = (cfg.sdMaxSpiHz > 0) ? cfg.sdMaxSpiHz : 20000000;
  if (sdDynCapHz && sdDynCapHz < capHz) capHz = sdDynCapHz;

  for (size_t i = 0; i < sizeof(kLadder) / sizeof(kLadder[0]); i++) {
    uint32_t hz = kLadder[i];
    if (hz > capHz) continue;  // skip entries above the configured cap

    if (SD.begin(csPin, SPI, hz)) {
      Serial.printf("[SD] Running at %lu Hz\n", (unsigned long)hz);
      sdRunHz = hz;
      return true;
    }
    SD.end();
  }

  // Last-resort attempt at the cap's own value in case it doesn't match a
  // ladder rung exactly (e.g. a user set sdMaxSpiHz=25000000).
  if (capHz > 0 && SD.begin(csPin, SPI, capHz)) {
    Serial.printf("[SD] Running at %lu Hz\n", (unsigned long)capHz);
    sdRunHz = capHz;
    return true;
  }

  return false;
}

// ---- SD bus recovery ----
// After sustained heavy SD I/O (large batch uploads at high SPI clock), the
// SD/SPI bus can wedge, causing SD.exists()/SD.open() to fail even though
// the underlying file is intact on the card. A full unmount + remount at
// the negotiated best clock usually clears it. Rate-limited so a genuinely
// dead card doesn't get hammered with a remount attempt on every file.
static uint32_t sdLastRecoverMs = 0;
bool sdTryRecover() {
  uint32_t now = millis();
  if (now - sdLastRecoverMs < 4000) return false;
  sdLastRecoverMs = now;

  // A wedge means the clock the card mounted at isn't reliable on this
  // card/wiring: remount one rung slower (never below 4 MHz). Lasts until reboot;
  // set sdMaxSpiHz in wardriver.cfg to make it permanent.
  for (size_t i = 0; i + 1 < sizeof(kLadder) / sizeof(kLadder[0]); i++) {
    if (sdRunHz && kLadder[i] <= sdRunHz) {
      if (kLadder[i + 1] >= 4000000) sdDynCapHz = kLadder[i + 1];
      break;
    }
  }
  Serial.printf("[SD] Attempting SD bus recovery (SD.end + reinit, cap %lu Hz)...\n", (unsigned long)sdDynCapHz);
  SD.end();
  delay(50);
  bool ok = sdBeginBestClock(pins.sd_cs);
  sdOk = ok;
  Serial.printf("[SD] Recovery %s\n", ok ? "OK" : "FAILED");
  return ok;
}

// ---- Move a file into an SD subfolder ----
// Shared by moveToUploaded()/moveToArchived(). destDir is an absolute folder
// such as "/uploaded" or "/archived" (no trailing slash). Tries SD.rename and
// falls back to copy+delete for picky SD libs.
static bool moveToFolder(const String& srcPath, const char* destDir) {
  if (!sdOk) return false;
  if (!SD.exists(srcPath)) {
    Serial.printf("[SD] moveToFolder: source missing, attempting recovery: %s\n", srcPath.c_str());
    if (!sdTryRecover() || !SD.exists(srcPath)) {
      Serial.printf("[SD] moveToFolder: source missing: %s\n", srcPath.c_str());
      return false;
    }
    Serial.println("[SD] moveToFolder: SD recovered, source found");
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
  File in = SD.open(srcPath, FILE_READ);
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
// batch of SD_BATCH_LINES rows instead of one SD write per network. A
// time-based safety flush (SD_BATCH_FLUSH_MS, driven by sdLogBufferTick() from
// loop()) drains a partial batch so rows are not lost when the device is
// powered off at the end of a run. Worst-case loss on an abrupt power-off is
// one partial batch.
//
// The buffer is a fixed char block from PSRAM when the board has it (the XIAO
// ESP32-C5 does — see board_build.arduino.memory_type in platformio.ini),
// freeing ~25 KB of scarce internal RAM, and falls back to internal RAM on any
// board without a usable SPIRAM region. Writing to SD from a PSRAM source is
// safe: FatFS copies into its own sector cache (no DMA-from-PSRAM constraint).
static const uint16_t SD_BATCH_LINES    = 100;     // rows per SD write
static const uint32_t SD_BATCH_FLUSH_MS = 20000;   // 15–30 s safety window
static const size_t   SD_ROW_MAX        = 256;     // worst-case bytes per row (+CRLF headroom)
static const size_t   SD_BATCH_CAP      = (size_t)SD_BATCH_LINES * SD_ROW_MAX;  // ~25 KB
static char*    s_logBuf      = nullptr; // batch buffer (PSRAM if available)
static size_t   s_logBufLen   = 0;       // bytes currently used
static uint16_t s_logBufLines = 0;       // rows currently buffered
static uint32_t s_bufStartMs  = 0;       // millis() of the first buffered row

// Allocate the batch buffer once: PSRAM first, then internal RAM, then plain
// malloc. Kept for the program's lifetime (reused across log rotations).
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

// Write the buffered rows to the open file in one batch, flush, and reset the
// counters. Does NOT rotate (flushLogBuffer handles that) so it is safe to call
// from closeLogFile() during shutdown/rotation without re-entering rotation.
static void drainLogBuffer() {
  if (s_logBufLines == 0) return;
  if (!sdOk || !logFile || !s_logBuf) { s_logBufLen = 0; s_logBufLines = 0; return; }

  size_t len = s_logBufLen;   // reset counters up front; bytes stay intact for retry
  s_logBufLen = 0;
  s_logBufLines = 0;

  size_t written = logFile.write((const uint8_t*)s_logBuf, len);
  csvBytes += (uint32_t)written;
  logFile.flush();
  yield();   // give WiFi/GPS/web-server/watchdog a chance after a big SD write

  static uint8_t consecFails = 0;
  if (len > 0 && written < len) {
    consecFails++;
    Serial.printf("[SD] Batch write short (%u/%u bytes, %u consecutive)\n",
                  (unsigned)written, (unsigned)len, consecFails);
    if (consecFails >= 3) {
      Serial.println("[SD] Attempting log reopen...");
      closeLogFile();            // buffer count already 0 — no recursion
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

// Drain the batch, then rotate the CSV if it has hit the size cap.
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

// Append one already-formatted row (without trailing newline) to the batch,
// writing the whole batch to SD once it fills.
static void bufferLogLine(const char* line, size_t len) {
  if (!sdOk || !logFile) return;

  if (!ensureLogBuffer()) {                 // alloc failed — write straight through
    size_t w = logFile.println(line);
    csvBytes += (uint32_t)w;
    logFile.flush();
    return;
  }

  if (len + 2 > SD_ROW_MAX) len = SD_ROW_MAX - 2;          // clamp pathological row
  if (s_logBufLen + len + 2 > SD_BATCH_CAP) flushLogBuffer();  // flush if it won't fit

  if (s_logBufLines == 0) s_bufStartMs = millis();         // start this batch's timer
  memcpy(s_logBuf + s_logBufLen, line, len);
  s_logBufLen += len;
  s_logBuf[s_logBufLen++] = '\r';
  s_logBuf[s_logBufLen++] = '\n';
  s_logBufLines++;

  if (s_logBufLines >= SD_BATCH_LINES) flushLogBuffer();
}

// Time-based safety flush. Call every loop(): drains a partial batch once its
// oldest row has waited longer than SD_BATCH_FLUSH_MS.
void sdLogBufferTick() {
  if (s_logBufLines == 0) return;
  if ((millis() - s_bufStartMs) >= SD_BATCH_FLUSH_MS) flushLogBuffer();
}

// Sanitise a user-provided device name for safe use in filenames.
// Keeps alphanumerics, hyphens, underscores; replaces spaces with _;
// strips everything else; truncates to 20 chars.

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
  else                              boardModel = "Xiao-ESP32S3";

  // WiGLE WiFi 1.6 header
  logFile.print("WigleWifi-1.6,appRelease=");
  logFile.print(FIRMWARE_VERSION);
  logFile.print(",model="); logFile.print(boardModel);
  logFile.print(",release=1,device="); logFile.print(deviceField);
  logFile.print(",display=SSD1306-128x64,board="); logFile.print(boardModel);
  logFile.println(",brand=WarHog,star=Sol,body=3,subBody=0");
  logFile.println("MAC,SSID,AuthMode,FirstSeen,Channel,Frequency,RSSI,CurrentLatitude,CurrentLongitude,AltitudeMeters,AccuracyMeters,RCOIs,MfgrId,Type");
  logFile.flush();
  // Seed the byte counter with the header size so the cap accounts for it.
  csvBytes = (uint32_t)logFile.size();

  Serial.println("[SD] Log file initialized with WiGLE headers");
  return true;
}

void closeLogFile() {
  drainLogBuffer();   // persist any buffered rows before the handle goes away
  if (logFile) {
    Serial.println("[SD] Closing log file");
    logFile.flush();
    logFile.close();
  }
}

// Copy `in` into fixed buffer `out` (size outSize), doubling any embedded
// double-quote characters per CSV escaping rules (RFC4180-style). Truncates
// safely rather than overflowing if the (unexpectedly long/malformed) input
// wouldn't fit.
static void csvEscapeQuotes(const String& in, char* out, size_t outSize) {
  size_t o = 0;
  size_t n = in.length();
  for (size_t i = 0; i < n; i++) {
    char c = in[i];
    if (c == '"') {
      if (o + 2 >= outSize) break;  // no room left for the doubled quote
      out[o++] = '"';
      out[o++] = '"';
    } else {
      if (o + 1 >= outSize) break;
      out[o++] = c;
    }
  }
  out[o] = '\0';
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

  // Size-cap rotation is handled at flush time (flushLogBuffer), after the
  // buffered batch is safely on the card.

  // Frequency in MHz derived from channel number (WiGLE 1.6 requirement)
  uint32_t freq = 0;
  if      (channel >= 1  && channel <= 13) freq = 2407u + (uint32_t)channel * 5;
  else if (channel == 14)                  freq = 2484u;
  else if (channel >= 32)                  freq = 5000u + (uint32_t)channel * 5;

  // Build the row into fixed stack buffers instead of chaining Arduino
  // String concatenation (each `+=` above used to allocate/copy/free on the
  // heap). This runs once per scan result and can fire hundreds of times
  // per batch in dense areas, so per-row heap churn adds up fast.
  char safeSsid[70];  // SSID is at most 32 bytes; worst case (all quotes) doubles to 64
  csvEscapeQuotes(ssid, safeSsid, sizeof(safeSsid));

  char line[256];
  int len = snprintf(line, sizeof(line),
                      "%s,\"%s\",%s,%s,%d,%u,%d,%.6f,%.6f,%.1f,%.1f,,,WIFI",
                      mac.c_str(), safeSsid, auth.c_str(), firstSeen.c_str(),
                      channel, (unsigned)freq, rssi, lat, lon, altM, accM);
  if (len < 0) {
    Serial.println("[SD] appendWigleRow: encoding error, row skipped");
    return false;
  }
  if ((size_t)len >= sizeof(line)) {
    Serial.printf("[SD] appendWigleRow: row truncated (needed %d bytes, buffer %u) — check SSID\n",
                  len, (unsigned)sizeof(line));
  }

  // Buffer the row; it reaches the card as part of the next batch write
  // (full batch, or the time-based safety flush). Silent-write-failure
  // detection and reopen/retry now live in drainLogBuffer().
  bufferLogLine(line, (size_t)len);
  return true;
}
