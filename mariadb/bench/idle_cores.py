"""Prints the logical CPUs ordered from the most idle, measured over a few seconds from /proc/stat.

    python3 -I idle_cores.py [SECONDS]      e.g. "11 4 9 ..." (the benchmark pins the server and the client to the first two)
"""
import sys
import time


def sample():
    cpus = {}
    for line in open('/proc/stat'):
        f = line.split()
        if f[0].startswith('cpu') and f[0] != 'cpu':
            v = list(map(int, f[1:9]))
            cpus[int(f[0][3:])] = (v[3] + v[4], sum(v))
    return cpus


def main():
    a = sample()
    time.sleep(float(sys.argv[1]) if len(sys.argv) > 1 else 3)
    b = sample()
    idle = {c: (b[c][0] - a[c][0]) / max(b[c][1] - a[c][1], 1) for c in a}
    print(' '.join(str(c) for c in sorted(idle, key=lambda c: -idle[c])))


main()
