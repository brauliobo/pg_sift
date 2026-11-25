#!/usr/bin/env python3 -I
"""Evil patterns on strings of growing length: the matcher of the SQLite and MariaDB hosts (sqlite/host/regex*.c; the driver
mariadb/tests/regex.c matches whole words) answers every one (no budget error) and its time grows linearly with the string, under ASan+UBSan.

    python3 -I sqlite/tests/regex_scaling.py
"""
import os, subprocess, sys, tempfile, time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
LENGTHS = (40, 64, 128, 256)
CASES = [   # pattern, word(L), expected answer
    ("(a+)+b", lambda n: "a" * n, "0"),
    ("^(a|aa)+$", lambda n: "a" * n + "b", "0"),
    ("(?=(a+)+b)x", lambda n: "a" * n, "0"),
    ("(?<=(a+)+b)x", lambda n: "a" * n + "x", "0"),
    ("(?!(a*)*c)a*d", lambda n: "a" * n, "0"),
    ("^abc", lambda n: "x" * n + "abc", "0"),
    ("(a|b)*c$", lambda n: "ab" * (n // 2), "0"),
    ("(?<=a*b)c", lambda n: "a" * n + "c", "0"),
    (".*(?<=a*b)c", lambda n: "a" * n + "bc", "1"),
]


def main():
    with tempfile.TemporaryDirectory(prefix="sift-rx-") as tmp:
        exe = os.path.join(tmp, "regex_test")
        host = os.path.join(ROOT, "host")
        subprocess.run(["gcc", "-std=gnu17", "-g", "-O1", "-fsanitize=address,undefined", "-fno-sanitize-recover=all", "-D_GNU_SOURCE",
                        "-DSIFT_HOST_SQLITE", "-I" + host, "-I" + os.path.join(ROOT, "..", "src"), "-o", exe,
                        os.path.join(ROOT, "..", "mariadb", "tests", "regex.c"), os.path.join(host, "regex.c"),
                        os.path.join(host, "regex_parse.c"), os.path.join(host, "regex_exec.c"), "-lm"], check=True)
        env = dict(os.environ, ASAN_OPTIONS="detect_leaks=0")
        bad = 0
        for pat, word, want in CASES:
            times = {}
            for n in LENGTHS:
                reps = 20
                inp = "".join(f"{pat}\t{word(n)}\n" for _ in range(reps))
                t0 = time.perf_counter()
                r = subprocess.run([exe, "match", "nosub"], input=inp, capture_output=True, text=True, env=env)
                times[n] = time.perf_counter() - t0
                if r.returncode != 0 or r.stdout.split() != [want] * reps:
                    print(f"FAIL {pat!r} L={n}: {r.stdout.split()[:3]} {r.stderr[:200]}")
                    bad += 1
            growth = times[256] / max(times[64], 1e-9)
            ok = growth < 12
            bad += not ok
            print(f"{'ok  ' if ok else 'FAIL'} {pat!r}: L=64 {times[64] * 1e3:.1f} ms, L=256 {times[256] * 1e3:.1f} ms (x{growth:.1f} for 4x the length)")
        sys.exit(1 if bad else 0)


main()
