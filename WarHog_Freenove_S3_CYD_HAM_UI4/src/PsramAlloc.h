#pragma once
// Zeroed array allocation that prefers PSRAM and falls back to internal RAM.
//
// Internal DRAM is the scarce resource on the S3 boards (Wi-Fi, BLE, TLS and SD
// DMA all need it), while most of the 8 MB PSRAM sits idle. This prebuilt
// framework has CONFIG_SPIRAM_ALLOW_BSS_SEG_EXTERNAL_MEMORY off, so large
// static buffers can't be placed in PSRAM with EXT_RAM_BSS_ATTR; allocate them
// instead, e.g.
//
//   static Slot* ring = psramCalloc<Slot>(N);             // was: static Slot ring[N];
//   static char (*rows)[LEN] = psramCalloc<char[LEN]>(N);  // was: static char rows[N][LEN];
//
// PSRAM is initialised before global constructors run, so static initialisers
// like the above are safe. Never use this for memory touched from IRAM_ATTR
// code (promiscuous-mode callbacks, ISRs): PSRAM is unreachable while the
// flash cache is disabled.
#include <stdlib.h>
#include "esp_heap_caps.h"

template <typename T>
static inline T* psramCalloc(size_t n) {
  void* p = heap_caps_calloc(n, sizeof(T), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!p) p = calloc(n, sizeof(T));
  return static_cast<T*>(p);
}
