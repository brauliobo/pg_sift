#!/usr/bin/env python3
"""The peak of anonymous resident memory of one process, or of the autovacuum workers of a postmaster, between its start and the creation of a stop file.

  rss-peak.py pid PID STOP OUT           the process PID
  rss-peak.py autovacuum POSTMASTER STOP OUT   every autovacuum worker that is a child of the postmaster

OUT gets one line, "BASE_KB PEAK_KB CPU_SECONDS": RssAnon at the first sample, its largest value, and the CPU time the process (pid mode) used between the first sample and the last. A sample every millisecond; OUT.ready exists once the first sample is taken, and the stop file ends it.
"""
import os
import sys
import time


def anon_kb(pid):
    try:
        with open(f"/proc/{pid}/status") as f:
            for line in f:
                if line.startswith("RssAnon:"):
                    return int(line.split()[1])
    except OSError:
        pass
    return None


def cpu_ticks(pid):
    try:
        with open(f"/proc/{pid}/stat") as f:
            rest = f.read().rsplit(")", 1)[1].split()
        return int(rest[11]) + int(rest[12])
    except OSError:
        return None


def workers(postmaster):
    out = []
    try:
        with open(f"/proc/{postmaster}/task/{postmaster}/children") as f:
            kids = f.read().split()
    except OSError:
        return out
    for pid in kids:
        try:
            with open(f"/proc/{pid}/cmdline", "rb") as f:
                if b"autovacuum worker" in f.read():
                    out.append(int(pid))
        except OSError:
            pass
    return out


def main():
    mode, target, stop, out = sys.argv[1], int(sys.argv[2]), sys.argv[3], sys.argv[4]
    base, peak, cpu0, cpu1 = None, 0, None, 0
    while not os.path.exists(stop):
        for pid in [target] if mode == "pid" else workers(target):
            kb = anon_kb(pid)
            if kb is None:
                continue
            if base is None:
                base = kb
                open(out + ".ready", "w").close()
            peak = max(peak, kb)
            if mode == "pid":
                cpu1 = cpu_ticks(pid) or cpu1
                cpu0 = cpu1 if cpu0 is None else cpu0
        time.sleep(0.001)
    with open(out, "w") as f:
        f.write(f"{base or 0} {peak} {((cpu1 - cpu0) if cpu0 is not None else 0) / os.sysconf('SC_CLK_TCK'):.3f}\n")


main()
