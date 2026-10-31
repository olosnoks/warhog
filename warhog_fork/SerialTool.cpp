#include "SerialTool.h"
#include "Globals.h"      // sdOk, logFile, FIRMWARE_VERSION
#include "Config.h"       // cfg, loadConfigFromSD()
#include "SDUtils.h"      // normalizeSdPath(), sdLogFlushNow()
#include <SD.h>
#include <FS.h>
#include <string.h>
#include <stdlib.h>
#include <fcntl.h>      // POSIX open/read for GET (see cmdGetPath)
#include <unistd.h>
#include <sys/stat.h>
#include "esp_attr.h"   // DMA_ATTR

// The XIAO C5 has no shared SD/TFT SPI bus, so no bus lock is needed here.
static inline void spiBusLock()   {}
static inline void spiBusUnlock() {}

// ---- session + PUT state ----
static bool     s_active   = false;
static uint32_t s_lastMs   = 0;
static const uint32_t TOOL_TIMEOUT_MS = 30000;

static File     s_pf;
static bool     s_putting  = false;
static bool     s_putCfg   = false;
static uint32_t s_putRemain= 0;
static uint32_t s_putCrc   = 0;
static uint32_t s_putAcc   = 0xFFFFFFFFu;
static char     s_putPath[160] = {0};

static const char* CFG_PATH     = "/wardriver.cfg";
static const char* CFG_TMP_PATH = "/wardriver.cfg.tmp";

static uint32_t crc32_run(uint32_t crc, const uint8_t* d, size_t n) {
  for (size_t i = 0; i < n; i++) {
    crc ^= d[i];
    for (int k = 0; k < 8; k++) crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
  }
  return crc;
}

static const char B64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
static size_t b64enc(const uint8_t* in, size_t n, char* out) {
  size_t o = 0, i = 0;
  while (i + 3 <= n) {
    uint32_t v = ((uint32_t)in[i] << 16) | ((uint32_t)in[i+1] << 8) | in[i+2];
    out[o++] = B64[(v>>18)&63]; out[o++] = B64[(v>>12)&63];
    out[o++] = B64[(v>>6)&63];  out[o++] = B64[v&63];
    i += 3;
  }
  size_t rem = n - i;
  if (rem == 1) {
    uint32_t v = (uint32_t)in[i] << 16;
    out[o++] = B64[(v>>18)&63]; out[o++] = B64[(v>>12)&63]; out[o++]='='; out[o++]='=';
  } else if (rem == 2) {
    uint32_t v = ((uint32_t)in[i] << 16) | ((uint32_t)in[i+1] << 8);
    out[o++] = B64[(v>>18)&63]; out[o++] = B64[(v>>12)&63]; out[o++] = B64[(v>>6)&63]; out[o++]='=';
  }
  out[o] = 0;
  return o;
}
static int b64val(char c) {
  if (c>='A'&&c<='Z') return c-'A';
  if (c>='a'&&c<='z') return c-'a'+26;
  if (c>='0'&&c<='9') return c-'0'+52;
  if (c=='+') return 62;
  if (c=='/') return 63;
  return -1;
}
static size_t b64dec(const char* in, uint8_t* out) {
  size_t o = 0; int quad[4], qn = 0;
  for (const char* p = in; *p; ++p) {
    if (*p == '=') continue;
    int v = b64val(*p);
    if (v < 0) continue;
    quad[qn++] = v;
    if (qn == 4) {
      out[o++] = (quad[0]<<2) | (quad[1]>>4);
      out[o++] = ((quad[1]&15)<<4) | (quad[2]>>2);
      out[o++] = ((quad[2]&3)<<6) | quad[3];
      qn = 0;
    }
  }
  if (qn >= 2) {
    out[o++] = (quad[0]<<2) | (quad[1]>>4);
    if (qn >= 3) out[o++] = ((quad[1]&15)<<4) | (quad[2]>>2);
  }
  return o;
}

static void putCleanup(bool removeFile) {
  if (s_pf) { spiBusLock(); s_pf.close(); spiBusUnlock(); }
  if (removeFile) {
    const char* p = s_putCfg ? CFG_TMP_PATH : s_putPath;
    spiBusLock(); if (p[0] && SD.exists(p)) SD.remove(p); spiBusUnlock();
  }
  s_putting = false;
}

static void startPut(const char* path, uint32_t size, uint32_t crc, bool isCfg) {
  if (!sdOk) { Serial.println("WT ERR nosd"); return; }
  if (s_putting) putCleanup(true);
  const char* wpath = isCfg ? CFG_TMP_PATH : path;
  strlcpy(s_putPath, path, sizeof(s_putPath));
  spiBusLock();
  if (SD.exists(wpath)) SD.remove(wpath);
  s_pf = SD.open(wpath, FILE_WRITE);
  spiBusUnlock();
  if (!s_pf) { Serial.println("WT ERR open"); return; }
  s_putting = true; s_putCfg = isCfg; s_putRemain = size; s_putCrc = crc; s_putAcc = 0xFFFFFFFFu;
  Serial.println("WT READY");
}

static void putData(const char* b64) {
  if (!s_putting) return;
  static uint8_t buf[600];
  size_t n = b64dec(b64, buf);
  if (n > s_putRemain) n = s_putRemain;
  spiBusLock();
  size_t w = s_pf.write(buf, n);
  spiBusUnlock();
  s_putAcc = crc32_run(s_putAcc, buf, n);
  if (w != n) { putCleanup(true); Serial.println("WT ERR write"); return; }
  s_putRemain -= n;
  if (s_putRemain == 0) {
    spiBusLock(); s_pf.flush(); s_pf.close(); spiBusUnlock();
    uint32_t got = s_putAcc ^ 0xFFFFFFFFu;
    if (got != s_putCrc) {
      const char* p = s_putCfg ? CFG_TMP_PATH : s_putPath;
      spiBusLock(); if (SD.exists(p)) SD.remove(p); spiBusUnlock();
      s_putting = false;
      Serial.printf("WT ERR crc want=%08lX got=%08lX\n", (unsigned long)s_putCrc, (unsigned long)got);
      return;
    }
    if (s_putCfg) {
      spiBusLock();
      if (SD.exists(CFG_PATH)) SD.remove(CFG_PATH);
      bool ok = SD.rename(CFG_TMP_PATH, CFG_PATH);
      spiBusUnlock();
      s_putting = false;
      if (!ok) { Serial.println("WT ERR rename"); return; }
      loadConfigFromSD();
      Serial.println("WT OK cfg");
      return;
    }
    s_putting = false;
    Serial.println("WT OK");
  }
}

// Reads go through POSIX read() into an internal, 64-byte-aligned DMA buffer
// instead of File::read(). File's 4 KiB stdio buffer can be allocated in PSRAM
// when internal RAM is tight; the SD driver then DMAs whole sectors into PSRAM
// and the CPU can read stale cached bytes (seen on the S3 CYD as a "corrupt"
// wardriver.cfg). The buffer is a multiple of 384 so each read() splits evenly
// into WT DATA lines.
static DMA_ATTR uint8_t s_getBuf[1536] __attribute__((aligned(64)));

static void cmdGetPath(const char* path) {
  if (!sdOk) { Serial.println("WT ERR nosd"); return; }
  sdLogFlushNow();
  String full = String(SD.mountpoint()) + path;
  struct stat st;
  int fd = -1;
  for (int attempt = 0; attempt < 2 && fd < 0; attempt++) {
    if (attempt == 1 && !sdTryRecover()) break;    // wedged bus: remount, try once more
    spiBusLock();
    fd = ::open(full.c_str(), O_RDONLY);
    if (fd >= 0 && (fstat(fd, &st) != 0 || S_ISDIR(st.st_mode))) { ::close(fd); fd = -1; }
    spiBusUnlock();
  }
  if (fd < 0) { Serial.println("WT ERR open"); return; }
  Serial.printf("WT BEGIN %lu\n", (unsigned long)st.st_size);
  char     b64[520];
  uint32_t acc = 0xFFFFFFFFu;
  const size_t want = (size_t)st.st_size;
  size_t   sent = 0;
  int      retries = 0;
  while (sent < want) {
    spiBusLock();
    int n = ::read(fd, s_getBuf, sizeof(s_getBuf));
    spiBusUnlock();
    if (n <= 0) {
      // Mid-file read error (marginal SD bus): remount, reopen at the same
      // offset and carry on -- the bytes already sent stay valid.
      if (retries++ >= 3) break;
      Serial.printf("[TK] GET read error at %u, recovering\n", (unsigned)sent);
      spiBusLock(); ::close(fd); spiBusUnlock();
      if (!sdTryRecover()) { delay(4100); sdTryRecover(); }   // wait out the 4 s rate limit once
      spiBusLock();
      fd = ::open(full.c_str(), O_RDONLY);
      if (fd >= 0 && lseek(fd, (off_t)sent, SEEK_SET) != (off_t)sent) { ::close(fd); fd = -1; }
      spiBusUnlock();
      if (fd < 0) break;
      continue;
    }
    for (int off = 0; off < n; off += 384) {
      int len = n - off < 384 ? n - off : 384;
      acc = crc32_run(acc, s_getBuf + off, len);
      b64enc(s_getBuf + off, len, b64);
      Serial.print("WT DATA ");
      Serial.println(b64);
    }
    sent += (size_t)n;
    yield();
  }
  if (fd >= 0) { spiBusLock(); ::close(fd); spiBusUnlock(); }
  if (sent != want) { Serial.printf("WT ERR read %u/%u\n", (unsigned)sent, (unsigned)want); return; }
  Serial.printf("WT END %08lX\n", (unsigned long)(acc ^ 0xFFFFFFFFu));
}

// After long runs the C5's SD/SPI bus can wedge: SD.open("/") still "works"
// but lists nothing and every file open fails, so the Toolkit showed an empty
// card. sdTryRecover() (SDUtils) remounts; the Toolkit commands now use it the
// same way the uploader does.

// Print one directory's entries. Returns the entry count, or -1 if the
// directory could not be opened. Prints no WT END / WT ERR.
static int lsOnce(const char* dir) {
  spiBusLock();
  File root = SD.open(dir);
  bool isDir = root && root.isDirectory();
  spiBusUnlock();
  if (!isDir) { if (root) { spiBusLock(); root.close(); spiBusUnlock(); } return -1; }
  int count = 0;
  while (true) {
    spiBusLock();
    File f = root.openNextFile();
    spiBusUnlock();
    if (!f) break;
    String p = normalizeSdPath(dir, f.name());
    bool d = f.isDirectory();
    uint32_t sz = d ? 0 : (uint32_t)f.size();
    spiBusLock(); f.close(); spiBusUnlock();
    if (d) Serial.printf("WT DIR %s\n", p.c_str());
    else   Serial.printf("WT F %lu %s\n", (unsigned long)sz, p.c_str());
    count++;
    yield();
  }
  spiBusLock(); root.close(); spiBusUnlock();
  return count;
}

// True when the card root can't be opened or lists nothing -- the wedged-bus
// signature (a real card always has at least wardriver.cfg or /logs).
static bool sdLooksWedged() {
  spiBusLock();
  File root = SD.open("/");
  bool ok = root && root.isDirectory();
  bool any = false;
  if (ok) { File f = root.openNextFile(); any = (bool)f; if (f) f.close(); }
  if (root) root.close();
  spiBusUnlock();
  return !ok || !any;
}

static void cmdLs(const char* dir) {
  if (!sdOk) { Serial.println("WT ERR nosd"); return; }
  if (!*dir) dir = "/";
  int n = lsOnce(dir);
  if ((n < 0 || (n == 0 && !strcmp(dir, "/"))) && sdTryRecover()) n = lsOnce(dir);
  Serial.println(n < 0 ? "WT ERR nodir" : "WT END");
}

static void cmdRm(const char* path) {
  if (!sdOk) { Serial.println("WT ERR nosd"); return; }
  if (!*path) { Serial.println("WT ERR nopath"); return; }
  // Never delete the log being written: FatFs would unlink an open file.
  if (currentCsvPath.length() && currentCsvPath == path) { Serial.println("WT ERR busy"); return; }
  spiBusLock();
  bool ok = SD.remove(path);
  if (!ok) ok = SD.rmdir(path);              // empty folder (the Toolkit deletes contents first)
  spiBusUnlock();
  Serial.println(ok ? "WT OK" : "WT ERR rm");
}

static void cmdPutFile(const char* args) {
  char path[160]; int i = 0;
  while (args[i] && args[i] != ' ' && i < 159) { path[i] = args[i]; i++; }
  path[i] = 0;
  const char* p = args + i; while (*p == ' ') p++;
  uint32_t size = strtoul(p, (char**)&p, 10); while (*p == ' ') p++;
  uint32_t crc  = strtoul(p, nullptr, 16);
  if (!path[0]) { Serial.println("WT ERR nopath"); return; }
  startPut(path, size, crc, false);
}

bool serialToolHandleLine(const char* line) {
  if (!(line[0] == 'W' && line[1] == 'T')) return false;
  if (!(line[2] == ' ' || line[2] == '\0')) return false;
  s_lastMs = millis();

  const char* rest = line + 2; while (*rest == ' ') rest++;
  char verb[12]; int vi = 0;
  while (rest[vi] && rest[vi] != ' ' && vi < 11) { verb[vi] = rest[vi]; vi++; }
  verb[vi] = 0;
  const char* args = rest + vi; while (*args == ' ') args++;

  if (s_putting) {
    if (!strcmp(verb, "DATA"))  { putData(args); return true; }
    if (!strcmp(verb, "ABORT")) { putCleanup(true); Serial.println("WT OK abort"); return true; }
    putCleanup(true);
  }

  if      (!strcmp(verb, "HELLO")) {
    s_active = true;
    if (sdOk && sdLooksWedged()) sdTryRecover();   // fresh session: clear a wedged bus up front
    Serial.printf("WT HELLO fw=%s model=%s proto=1\n", FIRMWARE_VERSION, cfg.board.c_str());
  }
  else if (!strcmp(verb, "LS"))    cmdLs(args);
  else if (!strcmp(verb, "GET"))   cmdGetPath(args);
  else if (!strcmp(verb, "PUT"))   cmdPutFile(args);
  else if (!strcmp(verb, "RM"))    cmdRm(args);
  else if (!strcmp(verb, "CFG"))   {
    char sub[8]; int si = 0;
    while (args[si] && args[si] != ' ' && si < 7) { sub[si] = args[si]; si++; }
    sub[si] = 0;
    const char* sa = args + si; while (*sa == ' ') sa++;
    if      (!strcmp(sub, "GET")) cmdGetPath(CFG_PATH);
    else if (!strcmp(sub, "PUT")) {
      uint32_t size = strtoul(sa, (char**)&sa, 10); while (*sa == ' ') sa++;
      uint32_t crc  = strtoul(sa, nullptr, 16);
      startPut(CFG_PATH, size, crc, true);
    } else Serial.println("WT ERR cfgsub");
  }
  else if (!strcmp(verb, "BYE"))   { if (s_putting) putCleanup(true); s_active = false; Serial.println("WT BYE"); }
  else if (verb[0] == 0)           Serial.println("WT PONG");
  else                             Serial.printf("WT ERR unknown %s\n", verb);
  return true;
}

bool serialToolActive() { return s_active; }

void serialToolTick() {
  if (!s_active) return;
  if (millis() - s_lastMs > TOOL_TIMEOUT_MS) {
    if (s_putting) putCleanup(true);
    s_active = false;
    Serial.println("WT TIMEOUT");
  }
}
