#!/usr/bin/env bash
# A file that uses a PostgreSQL symbol includes the header that declares it, itself: a header that another one pulled in is not a promise of the server, and PostgreSQL 19 dropped one
# (utils/timestamp.h: GetCurrentTimestamp in col/maint/net.c was an implicit declaration, an error of the hosted PostgreSQL 19 and package jobs, 2026-11-03). The table is scripts/pg-includes.tsv:
# a regular expression of the symbols, TAB, the headers that declare them (any of the list satisfies the rule), one row per family; a symbol a compile against a newer server found missing is a row.
#   scripts/check-pg-includes.sh            the files of src/ (a header counts for itself, not for the files that include it)
#   scripts/check-pg-includes.sh --selftest
set -euo pipefail
here=$(cd "$(dirname "$0")/.." && pwd)
exec python3 -I - "$here" "${1:-}" <<'PY'
import os, re, subprocess, sys, tempfile

root, arg = sys.argv[1], sys.argv[2]


def rows(path):
    out = []
    for n, l in enumerate(open(path), 1):
        if l.strip() and not l.startswith('#'):
            sym, _, hdrs = l.rstrip('\n').partition('\t')
            if not hdrs:
                sys.exit(f'{path}:{n}: a row is SYMBOLS TAB HEADERS')
            out.append((re.compile(r'\b(?:' + sym + r')\b'), hdrs.split()))
    return out


def bad(files, table):
    hits = []
    for f, text in files:
        code = re.sub(r'/\*.*?\*/|//[^\n]*', '', text, flags=re.S)
        have = set(re.findall(r'#\s*include\s+[<"]([^>"]+)[>"]', text))
        for rx, hdrs in table:
            m = rx.search(code)
            if m and not have & set(hdrs):
                hits.append((f, m.group(0), hdrs[0]))
    return hits


if arg == '--selftest':
    with tempfile.NamedTemporaryFile('w', suffix='.tsv') as t:
        t.write('GetCurrentTimestamp|TimestampTz\tutils/timestamp.h datatype/timestamp.h\n')
        t.flush()
        table = rows(t.name)
    use = '#include "postgres.h"\nvoid f(void) { TimestampTz t = GetCurrentTimestamp(); }\n'
    assert bad([('a.c', use)], table) == [('a.c', 'TimestampTz', 'utils/timestamp.h')], bad([('a.c', use)], table)
    assert not bad([('a.c', '#include "utils/timestamp.h"\n' + use)], table)
    assert not bad([('a.c', '#include "datatype/timestamp.h"\n' + use)], table)
    assert not bad([('a.c', '/* GetCurrentTimestamp */ int x; // TimestampTz\n')], table)
    print('check-pg-includes selftest: ok')
    sys.exit(0)

table = rows(os.path.join(root, 'scripts', 'pg-includes.tsv'))
names = subprocess.run(['git', '-C', root, 'ls-files', 'src/*.c', 'src/*.h'], capture_output=True, text=True, check=True).stdout.split()
files = [(f, open(os.path.join(root, f), errors='replace').read()) for f in names]
hits = bad(files, table)
for f, sym, h in hits:
    print(f'{f}: uses {sym} and does not include "{h}"')
print(f'check-pg-includes: {len(hits)} file(s) without the header of a symbol they use, {len(files)} files, {len(table)} rows')
sys.exit(1 if hits else 0)
PY
