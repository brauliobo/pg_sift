#!/usr/bin/env bash
# The limit registry (src/core/limits.h): docs/LIMITS.md is generated from it, and the registry is checked against the sources. Needs python3, no PostgreSQL.
#
#   scripts/gen-limits.sh           check the registry, then write docs/LIMITS.md
#   scripts/gen-limits.sh --check   check the registry and that docs/LIMITS.md is the generated one (lint)
#
# The checks fail when
#   - an object-like macro of src/ named *_MAX_* or *_LIMIT* is neither a row of the registry nor a line of scripts/limits.allow (with its reason),
#   - a row names a macro that its file does not define, or a literal value that differs from the #define,
#   - a line of limits.allow names a macro that does not exist (or is registered),
#   - a sift_limit(id, MACRO) site names an unknown id or a macro other than the row's, or a row is hooked without a site (or the reverse),
#   - the defaults the SIFT_TESTING build reports (test/expected/limits_registry.out) differ from the values evaluated here.
# (Include guards, names ending in _H, are skipped.)
# Values are evaluated from the #define bodies (the few PostgreSQL constants they use are listed below), so a macro written with another
# PostgreSQL macro needs it added to CONST.
set -euo pipefail

cd "$(dirname "$0")/.."
mode=${1:-write}
[[ $mode == write || $mode == --check ]] || { echo "usage: $0 [--check]" >&2; exit 2; }
exec python3 -I - "$mode" <<'PY'
import os
import re
import sys
sys.dont_write_bytecode = True
sys.path.insert(0, 'scripts')
from genlib import split_args, strip_comments

mode = sys.argv[1]
ROOT = os.getcwd()
SRC = os.path.join(ROOT, 'src')
CONST = {'CHAR_BIT': 8, 'FUNC_MAX_ARGS': 100, 'MaxHeapTuplesPerPage': 291, 'BLCKSZ': 8192, 'PG_UINT16_MAX': 65535,
         'PG_UINT32_MAX': 4294967295, 'PG_INT32_MAX': 2147483647}
LIMIT_NAME = re.compile(r'_MAX_|_LIMIT')
errors = []


def err(msg):
    errors.append(msg)


def read(path):
    with open(path, encoding='utf-8') as f:
        return f.read()


# ---- the sources: every #define, the sift_limit() sites ----
defs = {}      # name -> [(file from src/, line, body, function-like)]
sites = []     # (file, line, id, macro text)
for dirpath, _, files in os.walk(SRC):
    for fn in sorted(files):
        if not fn.endswith(('.c', '.h')):
            continue
        path = os.path.join(dirpath, fn)
        rel = os.path.relpath(path, SRC)
        text = strip_comments(read(path))
        lines = text.split('\n')
        i = 0
        while i < len(lines):
            start = i
            line = lines[i]
            while line.rstrip().endswith('\\') and i + 1 < len(lines):
                i += 1
                line = line.rstrip()[:-1] + ' ' + lines[i]
            i += 1
            m = re.match(r'\s*#\s*define\s+(\w+)(\()?\s*(.*)$', line)
            if m:
                defs.setdefault(m.group(1), []).append((rel, start + 1, m.group(3).strip(), bool(m.group(2))))
        if rel != 'core/limits.h':
            for m in re.finditer(r'\bsift_limit\(\s*(\w+)\s*,\s*([^;]*?)\)', text):
                sites.append((rel, text.count('\n', 0, m.start()) + 1, m.group(1), m.group(2).strip()))


def ev(expr, rel, depth=0):
    if depth > 12:
        raise ValueError('macro nesting too deep')
    e = re.sub(r'\bUINT64CONST\s*\(', '(', expr)
    e = re.sub(r'\bsizeof\s*\(\s*(?:uint32|int32)\s*\)', '4', e)
    e = re.sub(r'\(\s*(?:Size|uint32|uint64|int64|int|unsigned|long|double)\s*\)', '', e)
    e = re.sub(r'\b(0[xX][0-9a-fA-F]+|\d+)[uUlL]+\b', r'\1', e)
    e = re.sub(r'\bM(in|ax)\(', lambda m: 'm' + m.group(1) + '(', e)
    e = e.replace('/', '//')

    def sub(m):
        name = m.group(0)
        if name in ('min', 'max'):
            return name
        if name in CONST:
            return str(CONST[name])
        cands = [d for d in defs.get(name, []) if not d[3]]
        same = [d for d in cands if d[0] == rel]
        pick = same or cands
        if not pick or len({d[2] for d in pick}) != 1:
            raise ValueError('cannot evaluate %s (add it to CONST of scripts/gen-limits.sh)' % name)
        return '(' + str(ev(pick[0][2], pick[0][0], depth + 1)) + ')'
    e = re.sub(r'\b[A-Za-z_]\w*\b', sub, e)
    v = eval(e, {'__builtins__': {}}, {'min': min, 'max': max})
    return int(v)


# ---- the registry ----
def unq(s):
    return s[1:-1].replace('\\"', '"')


hdr = strip_comments(read(os.path.join(SRC, 'core', 'limits.h')))
block = hdr[hdr.index('#define SIFT_LIMITS(X)'):]
block = block[:block.index('\n\n')].replace('\\\n', ' ')
block = block[len('#define SIFT_LIMITS(X)'):]
rows = []
pos = 0
while True:
    k = block.find('X(', pos)
    if k < 0:
        break
    j, depth = k + 2, 1
    while depth:
        c = block[j]
        if c == '"':
            j += 1
            while block[j] != '"':
                j += 2 if block[j] == '\\' else 1
        elif c == '(':
            depth += 1
        elif c == ')':
            depth -= 1
        j += 1
    a = split_args(block[k + 2:j - 1])
    if len(a) != 9:
        err('limits.h: a row has %d columns, not 9: %s' % (len(a), a[0]))
    else:
        rows.append(dict(id=a[0], macro=unq(a[1]), file=unq(a[2]), value=a[3], unit=unq(a[4]), what=unq(a[5]), beh=unq(a[6]),
                         setting=unq(a[7]), hooked=int(a[8])))
    pos = j

ids = [r['id'] for r in rows]
for r in rows:
    if ids.count(r['id']) > 1:
        err('limits.h: id %s is twice in the registry' % r['id'])
    try:
        r['v'] = ev(r['value'], r['file'])
    except Exception as ex:
        err('limits.h: %s: %s' % (r['id'], ex))
        r['v'] = 0
    if r['macro'] != '-':
        d = [d for d in defs.get(r['macro'], []) if d[0] == r['file'] and not d[3]]
        if not d:
            err('limits.h: %s: %s is not defined in src/%s' % (r['id'], r['macro'], r['file']))
        else:
            try:
                if ev(d[0][2], r['file']) != r['v']:
                    err('limits.h: %s: the value %s differs from #define %s %s of src/%s' % (r['id'], r['value'], r['macro'], d[0][2], r['file']))
            except Exception as ex:
                err('limits.h: %s: %s' % (r['id'], ex))
    elif not os.path.exists(os.path.join(SRC, r['file'])):
        err('limits.h: %s: src/%s does not exist' % (r['id'], r['file']))

# ---- every limit macro has a row or a reason ----
registered = {r['macro'] for r in rows if r['macro'] != '-'}
allow = {}
for n, line in enumerate(read(os.path.join(ROOT, 'scripts', 'limits.allow')).split('\n'), 1):
    if not line.strip() or line.startswith('#'):
        continue
    name, _, why = line.partition('\t')
    if not why.strip():
        err('limits.allow:%d: %s has no reason (NAME, a tab, the reason)' % (n, name))
    if name in registered:
        err('limits.allow:%d: %s is a row of the registry' % (n, name))
    if not [d for d in defs.get(name, []) if not d[3]]:
        err('limits.allow:%d: %s is not defined in src/' % (n, name))
    allow[name] = why
for name, ds in sorted(defs.items()):
    if LIMIT_NAME.search(name) and not name.endswith('_H') and name not in registered and name not in allow and any(not d[3] for d in ds):
        d = [d for d in ds if not d[3]][0]
        err('src/%s:%d: %s is not in the registry: add a row to SIFT_LIMITS (src/core/limits.h), or a line with its reason to scripts/limits.allow' % (d[0], d[1], name))

# ---- the sites that can be lowered ----
by_id = {r['id']: r for r in rows}
seen = set()
for rel, line, rid, mac in sites:
    r = by_id.get(rid)
    if r is None:
        err('src/%s:%d: sift_limit(%s, ...) is not a row of the registry' % (rel, line, rid))
        continue
    seen.add(rid)
    if r['macro'] != '-' and mac != r['macro']:
        err('src/%s:%d: sift_limit(%s, %s): the row says %s' % (rel, line, rid, mac, r['macro']))
for r in rows:
    if r['hooked'] and r['id'] not in seen:
        err('limits.h: %s is hooked but no site reads it through sift_limit()' % r['id'])
    if not r['hooked'] and r['id'] in seen:
        err('limits.h: %s has a sift_limit() site but its row is not hooked' % r['id'])

# ---- the C side: the defaults the testing build reports are the values evaluated here ----
exp = os.path.join(ROOT, 'test', 'expected', 'limits_registry.out')
c_vals = {}
for line in read(exp).split('\n'):
    f = [x.strip() for x in line.split('|')]
    if len(f) == 5 and f[2].isdigit() and f[0] in by_id:
        c_vals[f[0]] = int(f[2])
for r in rows:
    if r['id'] not in c_vals:
        err('test/expected/limits_registry.out: %s is not listed (run the test and accept its output)' % r['id'])
    elif c_vals[r['id']] != r['v']:
        err('test/expected/limits_registry.out: %s is %d in the build but %d by scripts/gen-limits.sh' % (r['id'], c_vals[r['id']], r['v']))

# ---- docs/LIMITS.md ----
AREAS = [('agg', 'Aggregate pushdown (`src/agg`)'), ('col', 'Column index (`src/col`)'),
         ('core', 'Search index, storage and queries (`src/core`, `src/bm25`)'), ('table', '`sift_table` (`src/table`)')]


def area(r):
    d = r['file'].split('/')[0]
    return 'core' if d == 'bm25' else d


def num(r):
    v = r['v']
    s = '{:,}'.format(v)
    if r['unit'] in ('bytes', 'kB') and v >= 1024:
        b = v * (1024 if r['unit'] == 'kB' else 1)
        for p, u in ((30, 'GiB'), (20, 'MiB'), (10, 'KiB')):
            if b >= 1 << p:
                s += ' (%s%g %s)' % ('' if b % (1 << p) == 0 else '~', b / (1 << p), u)
                break
    return s


def gen():
    o = ['# Limits', '',
         'Generated by `scripts/gen-limits.sh` from the registry `SIFT_LIMITS` in `src/core/limits.h`; do not edit by hand.',
         '`scripts/gen-limits.sh --check` fails when this file is out of date or when a `*_MAX_*` / `*_LIMIT*` macro of `src/` has no row',
         '(or a reason in `scripts/limits.allow`).', '',
         '"Normal plan" means the statement runs on the plan PostgreSQL would choose without the scan, with the same result;',
         '`sift.explain_why` names the slug. A limit that is an error says so. Limits of PostgreSQL itself (1 GB per value, 1600 columns) are not listed.', '',
         'In a testing build (`make SIFT_TESTING=1`) `SET sift.test_limits = \'id=value,...\'` lowers the limits marked "yes" in the last column, so that a test reaches them',
         'on a few rows; `sift._test_limits()` lists every limit with its default and the value in force.', '']
    for key, title in AREAS:
        rs = [r for r in rows if area(r) == key]
        if not rs:
            continue
        o += ['## ' + title, '', '| id | Limit | Default | Unit | At the limit | Changed by | Source | Test |', '|---|---|---|---|---|---|---|---|']
        for r in rs:
            src = '`%s`' % r['file'] + (' `%s`' % r['macro'] if r['macro'] != '-' else '')
            ch = '`%s`' % r['setting'] if r['setting'] != '-' else ''
            o.append('| `%s` | %s | %s | %s | %s | %s | %s | %s |' % (r['id'], r['what'], num(r), r['unit'], r['beh'], ch, src, 'yes' if r['hooked'] else ''))
        o.append('')
    return '\n'.join(o)


doc = gen()
docpath = os.path.join(ROOT, 'docs', 'LIMITS.md')
if mode == '--check':
    if not os.path.exists(docpath) or read(docpath) != doc + '\n':
        err('docs/LIMITS.md is not what the registry generates: run scripts/gen-limits.sh')
elif not errors:
    with open(docpath, 'w', encoding='utf-8') as f:
        f.write(doc + '\n')
if errors:
    for e in errors:
        print('limits: ' + e, file=sys.stderr)
    sys.exit(1)
print('limits: ok, %d limits registered, %d macros with a reason to have no row' % (len(rows), len(allow)))
PY
