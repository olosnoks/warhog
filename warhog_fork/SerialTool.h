#pragma once
#include <Arduino.h>

// ================================================================
//  SerialTool — "WT" line protocol: SD file explorer + wardriver.cfg editor
//  over USB serial, driven by the web flasher's Piglet Toolkit.
//
//  Line-based so it coexists with debug logging: the host sends one
//  `WT <verb> ...\n` per command; every reply line starts with `WT ` so the
//  browser ignores interleaved debug output. Binary payloads are base64 in
//  `WT DATA <b64>` lines; each transfer is CRC32-verified.
//
//  Commands (host -> device):
//    WT HELLO                         -> WT HELLO fw=.. model=.. proto=1  (enters tool mode: pauses scanning)
//    WT LS <dir>                      -> WT F <size> <path> ... / WT DIR <path> ...  WT END
//    WT GET <path>                    -> WT BEGIN <size>  WT DATA <b64>..  WT END <crc32>
//    WT PUT <path> <size> <crc32>     -> WT READY ; host streams WT DATA <b64>.. -> WT OK | WT ERR <why>
//    WT RM <path>                     -> WT OK | WT ERR <why>
//    WT CFG GET                       -> (GET of /wardriver.cfg)
//    WT CFG PUT <size> <crc32>        -> WT READY ; host streams .. -> WT OK cfg | WT ERR
//    WT ABORT                         -> cancel an in-progress PUT
//    WT BYE                           -> WT BYE (leaves tool mode, scanning resumes)
//
//  Device control (same as the Web UI's /status.json, /start, /stop, /reboot):
//    WT STAT                          -> WT STAT {json}  (scan state, counts, GPS, Wi-Fi, upload result)
//    WT SCAN START | STOP             -> WT OK scan on|off  (START also leaves tool mode)
//    WT UPLOAD [all|wdgwars|wardrift] -> WT OK <uploaded> <total> <message> | WT ERR nohome|nowifi|nosd
//                                        (joins homeSsid if needed, drops it again afterwards)
//    WT REBOOT                        -> WT OK reboot, then the board restarts
// ================================================================

bool serialToolActive();
bool serialToolHandleLine(const char* line);
void serialToolTick();
