#!/usr/bin/env bash
# A shell that starts or reaches a PostgreSQL client names the cluster it means. A bare `psql`, `createdb`, `dropdb`... reaches whatever libpq defaults to, which on a developer
# machine is the system PostgreSQL (the socket in /run/postgresql, port 5432): databases made and dropped, and an ALTER SYSTEM, on a server that is not the test's (2026-11-03).
# A file passes when it names the cluster itself (PGHOST or PGPORT, scripts/pgtest.sh, test/t/lib.bash, which set them) or when the command has -h/--host/-p/--port/-D/host=; a file
# that takes the cluster from the environment of its caller (the benchmarks) is in scripts/pg-bare.allow with the reason, FILE TAB REASON.
#   scripts/check-pg-bare.sh            scripts/, test/, bench/, packaging/
#   scripts/check-pg-bare.sh --selftest
set -euo pipefail
here=$(cd "$(dirname "$0")/.." && pwd)
exec python3 -I - "$here" "${1:-}" <<'PY'
import os, re, subprocess, sys

root, arg = sys.argv[1], sys.argv[2]
CMD = re.compile(r'(?:^|[;&|`]|\$\()\s*(?:\w+=\S+\s+)*(?:exec\s+)?(psql|createdb|dropdb|createuser|dropuser|pg_ctl|pg_isready|pg_dump|pg_dumpall|pg_restore|pg_basebackup|vacuumdb|reindexdb)\b')
NAMED = re.compile(r'(^|\s)(-h|--host|-p|--port|-D)\b|host=|PGHOST|PGPORT|pgtest\.sh|\$SOCK')
FILE_NAMES = re.compile(r'PGHOST|PGPORT|pgtest\.sh|lib\.bash')


def bad(files, allow):
    hits = []
    for f, text in files:
        if f in allow or FILE_NAMES.search(text):
            continue
        for n, line in enumerate(text.splitlines(), 1):
            s = re.sub(r'\s#\s.*$', '', line.strip())
            if s.startswith('#') or not CMD.search(s) or NAMED.search(s):
                continue
            hits.append((f, n, s[:100]))
    return hits


if arg == '--selftest':
    bare = 'set -e\ncreatedb x\npsql -X -c "select 1"\n'
    assert len(bad([('a.sh', bare)], set())) == 2, bad([('a.sh', bare)], set())
    assert not bad([('a.sh', bare)], {'a.sh'}), 'an allowed file was refused'
    assert not bad([('a.sh', 'export PGHOST=$SOCK\n' + bare)], set()), 'a file that names the cluster was refused'
    assert not bad([('a.sh', 'psql -h "$SOCK" -c x\ndropdb -p 5 y\npg_ctl -D d start\npsql "host=/x dbname=y"\n')], set()), 'a command with a host was refused'
    assert not bad([('a.sh', '# psql is run by hand\necho "psql" # createdb\n')], set()), 'a comment or a word was refused'
    assert bad([('a.sh', 'x=$(psql -Atc "select 1")\n')], set()), 'a substitution passed'
    print('check-pg-bare selftest: ok')
    sys.exit(0)

allow = set()
path = os.path.join(root, 'scripts', 'pg-bare.allow')
for l in open(path):
    if l.strip() and not l.startswith('#'):
        f, _, why = l.rstrip('\n').partition('\t')
        if not why:
            sys.exit(f'{path}: a row is FILE TAB REASON: {l.strip()}')
        allow.add(f)
names = subprocess.run(['git', '-C', root, 'ls-files', 'scripts', 'test', 'bench', 'packaging'], capture_output=True, text=True, check=True).stdout.split()
names = [f for f in names if f.endswith(('.sh', '.bash'))]
files = [(f, open(os.path.join(root, f), errors='replace').read()) for f in names]
hits = bad(files, allow)
for f, n, s in hits:
    print(f'{f}:{n}: {s}  (names no cluster: PGHOST/PGPORT, -h/-p, scripts/pgtest.sh; or a row in scripts/pg-bare.allow)')
stale = sorted(allow - set(names))
for f in stale:
    print(f'scripts/pg-bare.allow: {f} is not a shell of the tree')
print(f'check-pg-bare: {len(hits)} command(s) that reach the default cluster, {len(names)} shells, {len(allow)} allowed')
sys.exit(1 if hits or stale else 0)
PY
