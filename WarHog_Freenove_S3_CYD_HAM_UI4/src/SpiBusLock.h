#pragma once
// ================================================================
//  Scheduler Phase 4 (Step 1, CYD) — shared SPI-bus lock.
//  On the Freenove S3 CYD the SD card and the ILI9341 TFT share ONE physical
//  SPI bus (pins 10-13; sdSpi(HSPI) is a separate *driver instance* on the
//  *same wires*). Once Core-mode SD logging moves to a core-0 task, the TFT
//  flush on core 1 (ScreenUI::present / getTouch) and the SD write on core 0
//  can collide on the bus. This recursive mutex serializes every shared-bus
//  access so the two cores never drive the wires at once.
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
