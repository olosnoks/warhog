#!/usr/bin/env bash
# Drop-in header drift guard.
#
# The PlatformIO board targets consume the scheduler headers directly via
# `-I../scheduler` (single source of truth). The LilyGo T-Dongle, however, is a
# flat Arduino IDE sketch: the IDE has no include-path mechanism, so the shared
# headers must physically live in the sketch folder ("DROP-IN ... copy into the
# sketch folder"). Those copies can silently drift from scheduler/ and ship a
# stale protocol to one board — exactly the failure this guard catches.
#
# Each copied header must stay byte-identical to its scheduler/ source.
# Exits non-zero (and prints a unified diff) on any divergence.
set -euo pipefail
cd "$(dirname "$0")/.."

# copy-path : canonical-source-under-scheduler/
PAIRS=(
  "TDongleC5_WarHog/pig_proto.h:pig_proto.h"
  "TDongleC5_WarHog/pig_dedup.h:pig_dedup.h"
  "TDongleC5_WarHog/bssid_cache.h:bssid_cache.h"
  "TDongleC5_WarHog/pig_dwell.h:pig_dwell.h"
)

fail=0
for pair in "${PAIRS[@]}"; do
  copy="${pair%%:*}"
  src="scheduler/${pair##*:}"
  if [ ! -f "$copy" ]; then echo "MISSING copy: $copy"; fail=1; continue; fi
  if [ ! -f "$src" ];  then echo "MISSING source: $src"; fail=1; continue; fi
  if diff -q "$src" "$copy" >/dev/null; then
    echo "OK   $copy == $src"
  else
    echo "DRIFT $copy != $src"
    diff -u "$src" "$copy" || true
    fail=1
  fi
done

if [ "$fail" -ne 0 ]; then
  echo
  echo "Drop-in copies have drifted from scheduler/. Re-sync with:"
  for pair in "${PAIRS[@]}"; do
    echo "  cp scheduler/${pair##*:} ${pair%%:*}"
  done
  exit 1
fi
echo "All drop-in headers in sync."
