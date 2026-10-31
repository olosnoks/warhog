#pragma once
#include <Arduino.h>

bool   openLogFile();
void   closeLogFile();
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
