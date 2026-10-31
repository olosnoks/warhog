#!/usr/bin/env python3
"""12-node scale sign-off for the adaptive mesh scheduler (DESIGN §13).

Reads an S3 Core's serial output (live, or a saved capture) and grades the run
against the Phase-4/5 acceptance targets at the target node count:

  * 0 RX deadline misses      -> [CORE-ISO] rx_drop txt/req never increase
  * 0 SD store drops          -> [CORE-ISO] drops never increase
  * RX jitter p95 < 5 ms      -> [CORE-ISO] rt_us max, p95 across windows
  * no crash / reboot / ENOMEM (abort, Guru Meditation, boot banner, err=0x101)
  * internal heap floor       -> [CORE-ISO] heap_min (needs the heap= fields)

Plus informational stats: node joins/timeouts, reassign count, [PAIR] overlap
per group, and free heap per node count (RAM is the binding constraint).

Live capture opens the port WITHOUT asserting DTR/RTS, so the Core is not
rebooted (pyserial's default open resets ESP32-S3 boards):

    python scripts/mesh_signoff.py --port COM7 --minutes 20 --save run.log
    python scripts/mesh_signoff.py --log run.log            # re-grade a capture

Exit code: 0 = PASS, 1 = FAIL, 2 = INCOMPLETE (target never reached / too short).
"""
import argparse
import re
import sys
import time
from collections import defaultdict

RE_ISO = re.compile(
    r"\[CORE-ISO\] SD store q: depth=(\d+) drops=(\d+) \| rx_drop txt=(\d+) req=(\d+)"
    r"(?: txtq_max=(\d+))? \| rt_us max=(\d+) gt5ms=(\d+) \| loop_us max=(\d+) gt5ms=(\d+)"
    r" \| nodes=(\d+)(?: \| heap=(\d+) heap_min=(\d+))?")
RE_PAIR = re.compile(r"\[PAIR\] grp=(\d+) overlap both=(\d+) either=(\d+) pct=(\d+)%")
RE_NEW = re.compile(r"\[CORE\] New \S+ node (\d+): ([0-9A-F:]{17})")
RE_TIMEOUT = re.compile(r"\[CORE\] Node (\d+) timed out")
RE_REASSIGN = re.compile(r"\[CORE\] Sub-cluster assign:")
RE_FULL = re.compile(r"\[CORE\] Max nodes reached")
RE_BOOT = re.compile(r"=== WarHog .* ===|=== Boot complete ===")
RE_CRASH = re.compile(r"Guru Meditation|abort\(\) was called|Backtrace:|rst:0x|Malloc failed")
RE_ENOMEM = re.compile(r"err=0x101|not enough mem")

ISO_PERIOD_S = 5.0


def p95(vals):
    if not vals:
        return None
    s = sorted(vals)
    return s[min(len(s) - 1, int(round(0.95 * (len(s) - 1))))]


class Run:
    def __init__(self, target):
        self.target = target
        self.iso = []            # dicts, in order
        self.pair = defaultdict(lambda: [0, 0])   # grp -> [both, either]
        self.joins = {}          # mac -> join count
        self.timeouts = 0
        self.reassigns = 0
        self.full = 0
        self.boots = 0
        self.crashes = []
        self.enomem = []
        self.lines = 0

    def feed(self, line):
        self.lines += 1
        m = RE_ISO.search(line)
        if m:
            g = [int(x) if x is not None else None for x in m.groups()]
            self.iso.append(dict(depth=g[0], drops=g[1], rx_txt=g[2], rx_req=g[3], txtq_max=g[4],
                                 rt_max=g[5], rt_gt5=g[6], loop_max=g[7], loop_gt5=g[8],
                                 nodes=g[9], heap=g[10], heap_min=g[11], boot=self.boots))
            return
        m = RE_PAIR.search(line)
        if m:
            grp, both, either = int(m[1]), int(m[2]), int(m[3])
            self.pair[grp][0] += both
            self.pair[grp][1] += either
            return
        m = RE_NEW.search(line)
        if m:
            self.joins[m[2]] = self.joins.get(m[2], 0) + 1
            return
        if RE_TIMEOUT.search(line):
            self.timeouts += 1
        elif RE_REASSIGN.search(line):
            self.reassigns += 1
        elif RE_FULL.search(line):
            self.full += 1
        elif RE_BOOT.search(line):
            if "Boot complete" not in line:
                self.boots += 1
        if RE_CRASH.search(line):
            self.crashes.append(line.strip())
        if RE_ENOMEM.search(line):
            self.enomem.append(line.strip())

    def report(self, min_minutes, heap_floor):
        at = [s for s in self.iso if s["nodes"] >= self.target]
        out = []
        verdicts = []   # (ok: True/False/None, label, detail)

        out.append(f"Lines read: {self.lines}   [CORE-ISO] samples: {len(self.iso)} "
                   f"(~{len(self.iso) * ISO_PERIOD_S / 60:.1f} min)")
        peak = max((s["nodes"] for s in self.iso), default=0)
        out.append(f"Peak nodes: {peak}   samples at >= {self.target} nodes: {len(at)} "
                   f"(~{len(at) * ISO_PERIOD_S / 60:.1f} min)")
        out.append(f"Distinct nodes joined: {len(self.joins)}   rejoins: "
                   f"{sum(c - 1 for c in self.joins.values())}   timeouts: {self.timeouts}   "
                   f"reassigns: {self.reassigns}   'Max nodes reached': {self.full}")
        flappers = sorted(((c, mac) for mac, c in self.joins.items() if c > 2), reverse=True)
        if flappers:
            out.append("Flapping nodes (joined >2x): " +
                       ", ".join(f"{mac} x{c}" for c, mac in flappers))

        dur_min = len(at) * ISO_PERIOD_S / 60
        if not at or dur_min < min_minutes:
            verdicts.append((None, "Reached target",
                             f"{dur_min:.1f} min at >= {self.target} nodes (need {min_minutes})"))
        else:
            verdicts.append((True, "Reached target",
                             f"{dur_min:.1f} min at >= {self.target} nodes"))

        # Counters are cumulative per boot; only count increases within one boot.
        def increase(key, samples):
            total, prev = 0, None
            for s in samples:
                if prev is not None and s["boot"] == prev["boot"] and s[key] > prev[key]:
                    total += s[key] - prev[key]
                prev = s
            return total

        if at:
            rx = increase("rx_txt", at) + increase("rx_req", at)
            verdicts.append((rx == 0, "0 RX deadline misses", f"rx_drop +{rx} at target"))
            sd = increase("drops", at)
            verdicts.append((sd == 0, "0 SD store drops", f"drops +{sd} at target"))
            rt = p95([s["rt_max"] for s in at])
            gt5 = sum(s["rt_gt5"] for s in at)
            verdicts.append((rt < 5000, "RX jitter p95 < 5 ms",
                             f"rt_us p95={rt} max={max(s['rt_max'] for s in at)} "
                             f"ticks>5ms={gt5}"))
            depth = max(s["depth"] for s in at)
            out.append(f"SD queue depth max at target: {depth}/48   "
                       f"loop_us p95={p95([s['loop_max'] for s in at])} (informational)")

        verdicts.append((not self.crashes and self.boots <= 1, "No crash / reboot",
                         f"{len(self.crashes)} crash lines, {self.boots} boot banners"
                         + (f"; first: {self.crashes[0][:80]}" if self.crashes else "")))
        verdicts.append((not self.enomem, "No SD ENOMEM", f"{len(self.enomem)} lines"))

        heaps = [s for s in self.iso if s["heap_min"] is not None]
        if heaps:
            low = min(s["heap_min"] for s in heaps)
            verdicts.append((low >= heap_floor, f"Internal heap floor >= {heap_floor} B",
                             f"lowest-ever free {low} B"))
            by_n = defaultdict(list)
            for s in heaps:
                by_n[s["nodes"]].append(s["heap"])
            out.append("Free internal heap by node count: " + "  ".join(
                f"{n}n={min(v)}" for n, v in sorted(by_n.items())))
        else:
            verdicts.append((None, "Internal heap floor",
                             "no heap= fields (firmware predates them; check 'v' by hand)"))

        if self.pair:
            out.append("[PAIR] overlap (lower = better complementary coverage): " + "  ".join(
                f"grp{g}={100 * b // e if e else 0}% ({e})"
                for g, (b, e) in sorted(self.pair.items())))

        out.append("")
        for ok, label, detail in verdicts:
            tag = {True: "PASS", False: "FAIL", None: "----"}[ok]
            out.append(f"  [{tag}] {label:32s} {detail}")
        if any(ok is False for ok, _, _ in verdicts):
            result, code = "FAIL", 1
        elif any(ok is None for ok, _, _ in verdicts[:1]):
            result, code = "INCOMPLETE", 2
        else:
            result, code = "PASS", 0
        out.append(f"\nSIGN-OFF: {result}")
        return "\n".join(out), code


def open_serial_no_reset(port, baud):
    try:
        import serial
    except ImportError:
        sys.exit("pyserial is required for --port (pip install pyserial)")
    s = serial.Serial()
    s.port, s.baudrate, s.timeout = port, baud, 0.5
    s.dtr = False   # set BEFORE open: asserting DTR/RTS on open resets the S3
    s.rts = False
    s.open()
    return s


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    src = ap.add_mutually_exclusive_group(required=True)
    src.add_argument("--port", help="Core serial port (e.g. COM7, /dev/ttyACM0)")
    src.add_argument("--log", help="grade a saved capture instead of a live port")
    ap.add_argument("--baud", type=int, default=115200)
    ap.add_argument("--minutes", type=float, default=20, help="live capture length")
    ap.add_argument("--save", help="also write the raw live capture to this file")
    ap.add_argument("--nodes", type=int, default=12, help="target node count")
    ap.add_argument("--min-at-target", type=float, default=10,
                    help="minutes that must be spent at the target node count")
    ap.add_argument("--heap-floor", type=int, default=1024,
                    help="lowest acceptable free internal heap, bytes")
    a = ap.parse_args()

    run = Run(a.nodes)
    if a.log:
        with open(a.log, encoding="utf-8", errors="replace") as f:
            for line in f:
                run.feed(line)
    else:
        ser = open_serial_no_reset(a.port, a.baud)
        save = open(a.save, "w", encoding="utf-8") if a.save else None
        end = time.time() + a.minutes * 60
        last_note = 0
        try:
            while time.time() < end:
                raw = ser.readline()
                if not raw:
                    continue
                line = raw.decode("utf-8", errors="replace")
                if save:
                    save.write(line)
                run.feed(line)
                if line.startswith("[CORE-ISO]") and time.time() - last_note > 60:
                    last_note = time.time()
                    print(f"{time.strftime('%H:%M:%S')} {line.strip()}", flush=True)
        except KeyboardInterrupt:
            print("\n(stopped early)")
        finally:
            ser.close()
            if save:
                save.close()

    text, code = run.report(a.min_at_target, a.heap_floor)
    print(text)
    sys.exit(code)


if __name__ == "__main__":
    main()
