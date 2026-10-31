#include "SerialTool.h"
#include "Globals.h"      // sdOk, logFile, FIRMWARE_VERSION, and the SD/FS backend
                          // (StorageCompat.h: SD on W550, SD_MMC-as-SD on the CYD)
#include "Config.h"       // cfg, loadConfigFromSD()
#include "SDUtils.h"      // normalizeSdPath(), sdLogFlushNow()
#include "SpiBusLock.h"   // spiBusLock()/spiBusUnlock() (no-op unless PIG_FEAT_CORE_ISO)
#include <string.h>
#include <stdlib.h>

// ---- session + PUT state ----
static bool     s_active   = false;
static uint32_t s_lastMs   = 0;
static const uint32_t TOOL_TIMEOUT_MS = 30000;

static File     s_pf;                 // open file during a PUT
static bool     s_putting  = false;
static bool     s_putCfg   = false;
static uint32_t s_putRemain= 0;
static uint32_t s_putCrc   = 0;       // expected
static uint32_t s_putAcc   = 0xFFFFFFFFu;
static char     s_putPath[160] = {0};

static const char* CFG_PATH     = "/wardriver.cfg";
static const char* CFG_TMP_PATH = "/wardriver.cfg.tmp";

// ---- CRC32 (poly 0xEDB88320), running form (seed 0xFFFFFFFF, finalize ^0xFFFFFFFF) ----
static uint32_t crc32_run(uint32_t crc, const uint8_t* d, size_t n) {
  for (size_t i = 0; i < n; i++) {
    crc ^= d[i];
    for (int k = 0; k < 8; k++) crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
  }
  return crc;
}

// ---- base64 ----
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

// ---- PUT helpers ----
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
  if (n > s_putRemain) n = s_putRemain;     // never exceed the declared size
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

// ---- GET ----
static void cmdGetPath(const char* path) {
  if (!sdOk) { Serial.println("WT ERR nosd"); return; }
  sdLogFlushNow();                          // make the active log consistent on SD
  spiBusLock();
  File f = SD.open(path, FILE_READ);
  spiBusUnlock();
  if (!f || f.isDirectory()) { if (f) f.close(); Serial.println("WT ERR open"); return; }
  Serial.printf("WT BEGIN %lu\n", (unsigned long)f.size());
  uint8_t raw[384];
  char    b64[520];
  uint32_t acc = 0xFFFFFFFFu;
  while (true) {
    spiBusLock();
    int n = f.read(raw, sizeof(raw));
    spiBusUnlock();
    if (n <= 0) break;
    acc = crc32_run(acc, raw, n);
    b64enc(raw, n, b64);
    Serial.print("WT DATA ");
    Serial.println(b64);
    yield();
  }
  spiBusLock(); f.close(); spiBusUnlock();
  Serial.printf("WT END %08lX\n", (unsigned long)(acc ^ 0xFFFFFFFFu));
}

// ---- LS ----
static void cmdLs(const char* dir) {
  if (!sdOk) { Serial.println("WT ERR nosd"); return; }
  if (!*dir) dir = "/";
  spiBusLock();
  File root = SD.open(dir);
  bool isDir = root && root.isDirectory();
  spiBusUnlock();
  if (!isDir) { if (root) { spiBusLock(); root.close(); spiBusUnlock(); } Serial.println("WT ERR nodir"); return; }
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
    yield();
  }
  spiBusLock(); root.close(); spiBusUnlock();
  Serial.println("WT END");
}

// ---- RM ----
static void cmdRm(const char* path) {
  if (!sdOk) { Serial.println("WT ERR nosd"); return; }
  if (!*path) { Serial.println("WT ERR nopath"); return; }
  spiBusLock();
  bool ok = SD.remove(path);
  spiBusUnlock();
  Serial.println(ok ? "WT OK" : "WT ERR rm");
}

// Parse "<path> <size> <crc-hex>" for PUT.
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

  // A PUT in progress: only DATA / ABORT are expected; anything else aborts it.
  if (s_putting) {
    if (!strcmp(verb, "DATA"))  { putData(args); return true; }
    if (!strcmp(verb, "ABORT")) { putCleanup(true); Serial.println("WT OK abort"); return true; }
    putCleanup(true);  // unexpected command cancels the transfer, then falls through
  }

  if      (!strcmp(verb, "HELLO")) { s_active = true; Serial.printf("WT HELLO fw=%s model=%s proto=1\n", FIRMWARE_VERSION, cfg.board.c_str()); }
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
  else if (verb[0] == 0)           Serial.println("WT PONG");  // bare "WT"
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
