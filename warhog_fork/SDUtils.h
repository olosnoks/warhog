#pragma once
#include <Arduino.h>

bool   openLogFile();
void   closeLogFile();
// Time-based safety flush for the batched CSV writer — call every loop() so a
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

#include "sd_paths.h"   // pathBasename, normalizeSdPath, isAllowedDataPath, sanitiseDeviceName (common/)
bool   moveToUploaded(const String& srcPath);
// Move a file into /archived instead of deleting it. Used by the empty-CSV
// auto-cleanup so the device never silently removes a file.
bool   moveToArchived(const String& srcPath);

// Negotiates the fastest working SD-over-SPI clock: tries a descending
// ladder of speeds (capped by cfg.sdMaxSpiHz) via SD.begin(csPin, SPI, hz)
// and stops at the first one that mounts successfully. Assumes SPI.begin()
// has already been called with the correct pins. Logs the negotiated speed.
bool sdBeginBestClock(uint8_t csPin);

// Attempts to recover a wedged SD/SPI bus after a transient failure (e.g.
// SD.exists()/SD.open() unexpectedly failing on a file that should be
// present, typically after sustained heavy SD I/O). Fully re-mounts the
// card via SD.end() + sdBeginBestClock() and updates the global sdOk flag.
// Rate-limited internally (~4s cooldown) so a genuinely dead card doesn't
// get hammered with a remount attempt on every single file. Returns true
// if the card responded again after recovery.
bool sdTryRecover();
