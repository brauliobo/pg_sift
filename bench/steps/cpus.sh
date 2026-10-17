#!/usr/bin/env bash
# Usage: bench/steps/cpus.sh [SERVER_THREADS [LOAD_THREADS]]   (default 8 and 4)
# Prints "<server cpus> <load generator cpus>" as two cpu lists (0-3,8-11): the CPU budget of a run, picked from the machine as it
# is now. The CPUs are taken a physical core at a time (all the hardware threads of a core go to the same side), the cores that
# are least busy over a one second sample first, so a core that another job keeps busy (a GPU server) is left alone, and the two
# lists are disjoint. On a machine too small for the request the threads are split 2:1. `bench --cpus LIST --load-cpus LIST` or
# CPUS / LOAD_CPUS in the environment override it.
set -euo pipefail
python3 -I - "${1:-8}" "${2:-4}" <<'PY'
import subprocess, sys, time

def stat():
    cpus = {}
    for l in open('/proc/stat'):
        f = l.split()
        if f[0].startswith('cpu') and f[0] != 'cpu':
            v = list(map(int, f[1:9]))
            cpus[int(f[0][3:])] = (sum(v) - v[3] - v[4], sum(v))   # busy, total (idle and iowait are not busy)
    return cpus

want_s, want_l = int(sys.argv[1]), int(sys.argv[2])
a = stat(); time.sleep(1); b = stat()
busy = {c: (b[c][0] - a[c][0]) / max(b[c][1] - a[c][1], 1) for c in b}
core = {}
for l in subprocess.run(['lscpu', '-p=cpu,core'], capture_output=True, text=True, check=True).stdout.splitlines():
    if not l.startswith('#'):
        cpu, c = map(int, l.split(','))
        core.setdefault(c, []).append(cpu)
cores = sorted(core, key=lambda c: (max(busy[x] for x in core[c]), c))
total = sum(map(len, core.values()))
if want_s + want_l > total:
    want_s, want_l = max(1, total * 2 // 3), max(1, total // 3)
chosen = []
for want in (want_s, want_l):
    got = []
    while len(got) < want and cores:
        got += sorted(core[cores.pop(0)])
    chosen.append(sorted(got[:want]) if len(got) > want else got)

def span(l):
    out, i = [], 0
    while i < len(l):
        j = i
        while j + 1 < len(l) and l[j + 1] == l[j] + 1:
            j += 1
        out.append(str(l[i]) if i == j else '%d-%d' % (l[i], l[j]))
        i = j + 1
    return ','.join(out)

print(span(chosen[0]), span(chosen[1]))
PY
