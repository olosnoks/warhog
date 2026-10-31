#pragma once
#include <Arduino.h>

bool   openLogFile();
void   closeLogFile();

// ---- SD bus recovery (Guition W550 SPI SD) ----
// sdBeginBestClock() mounts the card at the fastest stable clock on a ladder;
// sdTryRecover() remounts a wedged bus one rung slower. On the Freenove SD_MMC
// build both are no-ops (sdTryRecover() returns false) until an SD_MMC variant
// lands, so callers can invoke them unconditionally.
bool   sdBeginBestClock(uint8_t csPin);
bool   sdTryRecover();
// Time-based safety flush for the batched CSV writer — call every loop().
// Writes out buffered rows that have waited longer than the flush window so a
// partial batch is never lost on power-off.
void   sdLogBufferTick();

// Force every buffered row to the card and flush the file handle now (used by
// the serial toolkit so a download sees a consistent on-SD copy of the log).
void   sdLogFlushNow();
// Returns true if the row was actually written to the CSV, false if skipped
// (SD/log file unavailable, or the SSID matches a configured whitelist entry).
bool   appendWigleRow(const String& mac, const String& ssid, const String& auth,
                      const String& firstSeen, int channel, int rssi,
                      double lat, double lon, double altM, double accM);
void   appendWigleBleRow(const String& mac, const String& name, int rssi,
                         const String& firstSeen,
                         double lat, double lon, double altM, double accM);

String normalizeSdPath(const char* dir, const char* nameIn);
String pathBasename(const String& p);
bool   isAllowedDataPath(const String& p);
bool   moveToUploaded(const String& srcPath);
// Move a file into /archived instead of deleting it. Used by the empty-CSV
// auto-cleanup so the device never silently removes a file.
bool   moveToArchived(const String& srcPath);

// Read-only SD file that is safe to use whatever the heap looks like.
//
// Arduino File::read() goes through a 4 KiB stdio buffer. When internal RAM is
// tight that buffer is allocated in PSRAM, the SD driver DMAs whole sectors
// straight into it, and the CPU can read stale cached bytes: files come back
// with chunks of whatever that memory held before. SdReader reads with POSIX
// read() through one shared, 64-byte-aligned internal DMA buffer instead, so
// every read is correct. It covers the File subset the firmware uses for
// reading: if (!f), size(), read(buf, n), read(), available(),
// readStringUntil(), close(). Writes through File are unaffected.
class SdReader {
 public:
  explicit SdReader(const char* path);
  explicit SdReader(const String& path) : SdReader(path.c_str()) {}
  ~SdReader() { close(); }
  SdReader(const SdReader&) = delete;
  SdReader& operator=(const SdReader&) = delete;

  explicit operator bool() const { return _fd >= 0; }
  size_t size() const { return _size; }
  int    available() const;
  int    read(uint8_t* dst, size_t n);   // bytes read, 0 at end of file
  int    read();                         // one byte, or -1 at end of file
  String readStringUntil(char term);     // like Stream::readStringUntil (term dropped)
  void   close();

 private:
  bool   fill();                         // refill _ra from the file
  int    rawRead(uint8_t* dst, size_t n);
  int      _fd = -1;
  size_t   _size = 0, _pos = 0;          // file size, underlying fd offset
  uint8_t  _ra[128];                     // small read-ahead for line/byte reads
  uint16_t _raLen = 0, _raPos = 0;
};
