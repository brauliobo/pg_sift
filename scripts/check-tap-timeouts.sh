#!/usr/bin/env bash
# A tap test that cancels or gives up at a short clock cuts the product wait it checks at a load-dependent point: on a loaded host the statement is slow for
# reasons of the host, and the test fails for a reason that is in no commit. A `statement_timeout`, `lock_timeout` or shell `timeout` literal of 10 s or less in test/t
# needs `# timeout-ok: <what bounds the product side>` on the same line (the bound is the point of the test: a lock_timeout the product has to honour, a cancel that is
# the test); any other wait polls an observable condition (wait_for in test/t/lib.bash, SiftTest.pm) with a large cap.
#   scripts/check-tap-timeouts.sh            test/t/
#   scripts/check-tap-timeouts.sh --selftest
set -euo pipefail
here=$(cd "$(dirname "$0")/.." && pwd)
exec python3 -I - "$here" "${1:-}" <<'PY'
import os, re, subprocess, sys

root, arg = sys.argv[1], sys.argv[2]
LIMIT = 10
UNIT = {'us': 1e-6, 'ms': 1e-3, 's': 1, 'min': 60, 'h': 3600, 'd': 86400, 'm': 60}
SETTING = re.compile(r"\b(?:statement|lock)_timeout\b\s*(?:=|TO)\s*'?(\d+(?:\.\d+)?)\s*(us|ms|s|min|h|d)?'?")
COMMAND = re.compile(r"(?:^|[\s;&|(`])timeout\s+(?:-\S+\s+)*(\d+(?:\.\d+)?)([smhd])?(?=\s)")


def short(line):
    # 0 is no timeout
    secs = [float(m[1]) * UNIT[m[2] or 'ms'] for m in SETTING.finditer(line)]
    secs += [float(m[1]) * UNIT[m[2] or 's'] for m in COMMAND.finditer(line)]
    return any(0 < s <= LIMIT for s in secs)


def bad(files):
    hits = []
    for f, text in files:
        for n, line in enumerate(text.splitlines(), 1):
            if not line.lstrip().startswith('#') and short(line) and not re.search(r'#\s*timeout-ok:\s*\S', line):
                hits.append((f, n, line.strip()[:110]))
    return hits


if arg == '--selftest':
    one = lambda s: bad([('a.sh', s + '\n')])
    for s in ["sql n \"SET statement_timeout = 2000\" x", "SET lock_timeout = '300ms'", "SET statement_timeout TO '10s'", "timeout 5 psql -c x",
              "x=$(timeout -k 1 10 psql)", "statement_timeout = 9.5s", "SET statement_timeout = '1min' ; SET lock_timeout = 1"]:
        assert one(s), f'a short timeout passed: {s}'
    for s in ["SET statement_timeout = '30s'", "SET statement_timeout = 11000", "timeout 120 bash -c x", "SET statement_timeout = 0", "SET lock_timeout = $t",
              "SET statement_timeout = $((RANDOM % 40 + 1))", "deadlock_timeout = 200ms", "checkpoint_timeout = 30s", "# SET lock_timeout = '1s'",
              "SET lock_timeout = '1s' # timeout-ok: the lock timeout is the bound the cut honours", "timeout() { :; }", "diag \"lock_timeout 500 ms\""]:
        assert not one(s), f'a timeout that needs no note was refused: {s}'
    assert one("SET lock_timeout = '1s' # timeout-ok:"), 'an empty note passed'
    print('check-tap-timeouts selftest: ok')
    sys.exit(0)

names = subprocess.run(['git', '-C', root, 'ls-files', 'test/t'], capture_output=True, text=True, check=True).stdout.split()
files = [(f, open(os.path.join(root, f), errors='replace').read()) for f in names]
hits = bad(files)
for f, n, s in hits:
    print(f'{f}:{n}: {s}  (a timeout of {LIMIT} s or less: append "# timeout-ok: <what bounds the product side>", or poll a condition with wait_for)')
print(f'check-tap-timeouts: {len(hits)} short timeout(s) without a note, {len(names)} files')
sys.exit(1 if hits else 0)
PY
