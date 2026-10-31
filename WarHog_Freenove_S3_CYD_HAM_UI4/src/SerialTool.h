#pragma once
#include <Arduino.h>

// ================================================================
//  SerialTool — "WT" line protocol: SD file explorer + wardriver.cfg editor
//  over USB serial, driven by the web flasher's Piglet Toolkit.
//
//  Line-based so it coexists with the firmware's debug logging: the host sends
//  one `WT <verb> ...\n` per command; every reply line starts with `WT ` so the
//  browser ignores interleaved debug output. Binary payloads are base64 in
//  `WT DATA <b64>` lines; each transfer is CRC32-verified.
//
//  Commands (host -> device):
//    WT HELLO                         -> WT HELLO fw=.. model=.. proto=1  (enters tool mode: pauses scanning)
//    WT LS <dir>                      -> WT F <size> <path> ...  WT END
//    WT GET <path>                    -> WT BEGIN <size>  WT DATA <b64>..  WT END <crc32>
//    WT PUT <path> <size> <crc32>     -> WT READY ; host streams WT DATA <b64>.. -> WT OK | WT ERR <why>
//    WT RM <path>                     -> WT OK | WT ERR <why>
//    WT CFG GET                       -> (GET of /wardriver.cfg)
//    WT CFG PUT <size> <crc32>        -> WT READY ; host streams .. -> WT OK (config reloaded) | WT ERR
//    WT ABORT                         -> cancel an in-progress PUT
//    WT BYE                           -> WT BYE (leaves tool mode, scanning resumes)
// ================================================================

// True while a tool session holds the device (loop() should pause scanning).
bool serialToolActive();

// Handle one complete input line (no trailing CR/LF). Returns true if the line
// was a tool-protocol line (started with "WT") and was consumed.
bool serialToolHandleLine(const char* line);

// Call every loop(): enforces the inactivity timeout (auto-exit tool mode).
void serialToolTick();
