#include "SDUtils.h"
#include "Globals.h"

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

// ---- Move to uploaded ----

bool moveToUploaded(const String& srcPath) {
  if (!sdOk) return false;
  if (!SD.exists(srcPath)) {
    Serial.print("[SD] moveToUploaded: source missing: ");
    Serial.println(srcPath);
    return false;
  }

  // Ensure folder exists
  if (!SD.exists("/uploaded")) {
    Serial.println("[SD] Creating /uploaded ...");
    if (!SD.mkdir("/uploaded")) {
      Serial.println("[SD] ERROR: SD.mkdir(/uploaded) failed");
      return false;
    }
  }

  String dstPath = String("/uploaded/") + pathBasename(srcPath);

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

  bool ok = SD.rename(srcPath, dstPath);
  if (!ok) {
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

  Serial.println("[SD] Move OK");
  return true;
}

// ---- Log file ----

// Hard size cap per CSV file: keep every log safely under the 30 MB upload
// limit so each file can always be uploaded. This is byte-based (not a
// row-count estimate) so the ceiling holds regardless of row width — long
// SSIDs or RCOIs can't push a file over. The ~1 MB of headroom below 30 MB
// absorbs the final row plus the next file's header. When the cap is reached,
// the active CSV is closed and a fresh one is opened with proper WiGLE headers.
static const uint32_t CSV_MAX_BYTES = 29u * 1024u * 1024u;  // < 30 MB
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
static const uint16_t SD_BATCH_LINES    = 100;     // rows per SD write
static const uint32_t SD_BATCH_FLUSH_MS = 20000;   // 15–30 s safety window
static String   s_logBuf;                // accumulated "row\r\n" text
static uint16_t s_logBufLines = 0;       // rows currently buffered
static uint32_t s_bufStartMs  = 0;       // millis() of the first buffered row

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

  // Pre-size the batch buffer so accumulating a full batch doesn't repeatedly
  // realloc/fragment the heap (one WiGLE row averages ~140 B).
  s_logBuf.reserve((size_t)SD_BATCH_LINES * 160u);

  Serial.println("[SD] Log file initialized with WiGLE headers");
  return true;
}

// Write the buffered rows to the open file in one batch, flush, and clear the
// buffer. Does NOT rotate the CSV (flushLogBuffer() handles that afterwards) so
// it is safe to call from closeLogFile() during shutdown/rotation without
// re-entering the rotation path.
static void drainLogBuffer() {
  if (s_logBufLines == 0) return;
  if (!sdOk || !logFile) { s_logBuf = ""; s_logBufLines = 0; return; }

  // Snapshot and clear up front: the reopen path below closes/reopens the log
  // file (which calls back into this function), and starting from an empty
  // buffer means that call is a no-op instead of corrupting a half-written batch.
  String batch = s_logBuf;
  s_logBuf = "";
  s_logBufLines = 0;
  s_logBuf.reserve((size_t)SD_BATCH_LINES * 160u);

  size_t want    = batch.length();
  size_t written = logFile.print(batch);
  csvBytes += (uint32_t)written;
  logFile.flush();   // commit the batch to the card now, not on a later timer

  // Silent write failure — print() returns short. Reopen the log once; if that
  // also fails, mark SD unusable until next boot (matches the old per-line path).
  static uint8_t consecFails = 0;
  if (want > 0 && written < want) {
    consecFails++;
    Serial.printf("[SD] Batch write short (%u/%u bytes, %u consecutive)\n",
                  (unsigned)written, (unsigned)want, consecFails);
    if (consecFails >= 3) {
      Serial.println("[SD] Attempting log reopen...");
      closeLogFile();              // buffer already empty here — no recursion
      if (openLogFile()) {
        Serial.println("[SD] Reopen OK — retrying batch");
        logFile.print(batch);      // best-effort retry into the fresh file
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

  if (s_logBufLines == 0) s_bufStartMs = millis();  // start this batch's timer
  s_logBuf += line;
  s_logBuf += "\r\n";
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
