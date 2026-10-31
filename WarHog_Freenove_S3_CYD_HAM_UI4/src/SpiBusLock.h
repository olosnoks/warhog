#pragma once
// ================================================================
//  Scheduler Phase 4 (Step 1, CYD) — cross-core SD access lock.
//  Once Core-mode SD logging moves to a core-0 task (tStore), core-1 SD users
//  (upload file moves, config saves, mesh-profile load/save) can run a
//  multi-step SD sequence while that task writes. This recursive mutex
//  serializes every SD access across the two cores.
//  Neither S3 board puts the SD card and the TFT on one SPI bus (Freenove:
//  ILI9341 on SPI 10-13, SD on 4-bit SDIO 38-48; W550: RGB panel, SD on SPI
//  10-13), so ScreenUI::present() taking it is purely defensive; it would only
//  matter on a board where the two genuinely share wires.
//  Flag-gated PIG_FEAT_CORE_ISO: OFF (default) -> lock/unlock compile to
//  no-ops, zero behavior change. The mutex itself is created in MeshNode
//  coreStoreStart() (before the SD task can run) and lives for the session.
// ================================================================
#ifndef PIG_FEAT_CORE_ISO
#define PIG_FEAT_CORE_ISO 0
#endif

#if PIG_FEAT_CORE_ISO
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
extern SemaphoreHandle_t gSpiBusMutex;   // defined + created in MeshNode.cpp
static inline void spiBusLock()   { if (gSpiBusMutex) xSemaphoreTakeRecursive(gSpiBusMutex, portMAX_DELAY); }
static inline void spiBusUnlock() { if (gSpiBusMutex) xSemaphoreGiveRecursive(gSpiBusMutex); }
#else
static inline void spiBusLock()   {}
static inline void spiBusUnlock() {}
#endif

// RAII scope guard — serializes a whole multi-op SD/TFT sequence (e.g. a
// file move with several SD.exists/rename/remove calls) across one critical
// section, auto-unlocking on every return path. No-op when PIG_FEAT_CORE_ISO
// is OFF, so zero behavior change. The mutex is recursive, so nesting a guard
// inside an already-held lock (e.g. SdReader's per-read lock) is safe.
struct SpiBusGuard { SpiBusGuard() { spiBusLock(); } ~SpiBusGuard() { spiBusUnlock(); } };
