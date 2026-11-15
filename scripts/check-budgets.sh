#!/usr/bin/env bash
# Every entry point of the tests and benchmarks has a budget (scripts/budgets.tsv: wall seconds and peak tmpfs MB) and runs under it (scripts/budget.sh).
#   - a script in scripts/ that starts a cluster or a workload (it names pgtest.sh, pgbench, pg_ctl or initdb; the check-* lint scripts excepted) and a fuzz driver in test/fuzz
#     needs a row `script:NAME` / `fuzz:NAME`: a new runner states how long it may take before it is merged. A helper that has no run of its own says so in its row (wall and tmpfs `-`).
#   - a row names a file that exists (a deleted runner takes its row with it); tap:NAME names a test/t file; the TAP files without a row run under tap:default.
#   - a single run is at most 1800 s, a gate (ci.sh, release.sh, stability.sh, gate:*) at most 2700 s, tmpfs at most 3072 MB.
#   scripts/check-budgets.sh [--selftest]
set -euo pipefail
here=$(cd "$(dirname "$0")/.." && pwd)
exec python3 -I - "$here" "${1:-}" <<'PY'
import glob, os, re, subprocess, sys

root, arg = sys.argv[1], sys.argv[2]
RUN, GATE, MB = 1800, 2700, 3072
GATES = {'script:ci.sh', 'script:release.sh', 'script:stability.sh'}
STARTS = re.compile(r'pgtest\.sh|pgbench|pg_ctl|initdb')


def problems(files, rows):
    """files: {path: text} of scripts/, test/fuzz and test/t; rows: {name: (wall, mb, what)}."""
    out = []
    for name, (wall, mb, what) in rows.items():
        helper = what.startswith('a helper')
        if (wall == '-') != helper or (mb == '-') != helper:
            out.append(f'{name}: wall and tmpfs are `-` only for a helper (what: "a helper ...")')
            continue
        if helper:
            continue
        cap = GATE if name in GATES or name.startswith('gate:') else RUN
        if int(wall) > cap or int(mb) > MB:
            out.append(f'{name}: {wall} s / {mb} MB is over the cap ({cap} s, {MB} MB): cut the work (smaller data, fewer seeds a run, shards), do not raise the row')
        kind, _, target = name.partition(':')
        path = {'script': f'scripts/{target}', 'fuzz': f'test/fuzz/{target}'}.get(kind)
        if path and path not in files:
            out.append(f'{name}: {path} does not exist (a deleted runner takes its row with it)')
        if kind == 'tap' and target != 'default' and not any(f'test/t/{target}.{e}' in files for e in ('sh', 'pl')):
            out.append(f'{name}: test/t/{target}.sh does not exist')
    for path, text in files.items():
        base = os.path.basename(path)
        if path.startswith('scripts/') and path.count('/') == 1 and path.endswith(('.sh', '.py')) and not base.startswith('check-') and STARTS.search(text):
            if f'script:{base}' not in rows:
                out.append(f'{path}: starts clusters or workloads and has no row `script:{base}` in scripts/budgets.tsv (wall seconds, tmpfs MB, the measurement; at most {RUN} s)')
        if path.startswith('test/fuzz/') and path.endswith('.py') and path.count('/') == 2 and "__main__" in text and f'fuzz:{base}' not in rows:
            out.append(f'{path}: a fuzz driver with no row `fuzz:{base}` in scripts/budgets.tsv')
    return out


def table(text):
    rows = {}
    for line in text.splitlines():
        if line and not line.startswith('#'):
            f = line.split('\t')
            rows[f[0]] = (f[1], f[2], f[3] if len(f) > 3 else '')
    return rows


if arg == '--selftest':
    ok = {'script:soak.sh': ('900', '1500', 'x')}
    files = {'scripts/soak.sh': 'pgbench', 'scripts/check-x.sh': 'pgtest.sh', 'scripts/lib.sh': 'echo', 'test/fuzz/f.py': "__main__", 'test/t/a.sh': ''}
    assert not problems(files, {**ok, 'fuzz:f.py': ('600', '500', 'x'), 'tap:a': ('300', '500', 'x'), 'tap:default': ('1200', '2000', 'x')}), 'a clean table was refused'
    for why, f, r in [
        ('a runner without a row', files, {'fuzz:f.py': ('1', '1', 'x')}),
        ('a fuzz driver without a row', files, ok),
        ('a row of a deleted script', files, {**ok, 'fuzz:f.py': ('1', '1', 'x'), 'script:gone.sh': ('1', '1', 'x')}),
        ('a run over the cap', files, {**ok, 'fuzz:f.py': ('1801', '1', 'x')}),
        ('a gate over the cap', files, {**ok, 'fuzz:f.py': ('1', '1', 'x'), 'gate:g': ('2701', '1', 'x')}),
        ('tmpfs over the cap', files, {**ok, 'fuzz:f.py': ('1', '3073', 'x')}),
        ('a dash without a helper', files, {**ok, 'fuzz:f.py': ('-', '-', 'x')}),
        ('a tap row of no file', files, {**ok, 'fuzz:f.py': ('1', '1', 'x'), 'tap:b': ('1', '1', 'x')}),
    ]:
        assert problems(files, r), f'not refused: {why}'
    print('check-budgets selftest: ok')
    sys.exit(0)

names = subprocess.run(['git', '-C', root, 'ls-files', 'scripts', 'test/fuzz', 'test/t'], capture_output=True, text=True, check=True).stdout.split()
files = {f: open(os.path.join(root, f), errors='replace').read() for f in names if os.path.isfile(os.path.join(root, f))}
rows = table(open(os.path.join(root, 'scripts/budgets.tsv')).read())
bad = problems(files, rows)
print('\n'.join(bad))
print(f'check-budgets: {len(bad)} problem(s), {len(rows)} rows; {sum(1 for f in files if f.startswith("test/t/") and f.endswith((".sh", ".pl")))} TAP files, those without a row run under tap:default')
sys.exit(1 if bad else 0)
PY
