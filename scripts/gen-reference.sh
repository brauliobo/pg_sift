#!/usr/bin/env bash
# The SQL reference: every user-facing object of schema sift, read from the install scripts. docs/REFERENCE.md and the block of README.md between
# the sift-reference markers are generated. Needs python3 and no PostgreSQL (a second or less); --live asks a server.
#
#   scripts/gen-reference.sh           check, then write docs/REFERENCE.md and the README block
#   scripts/gen-reference.sh --check   check, and that both files are the generated ones (lint, scripts/ci.sh)
#   scripts/gen-reference.sh --live    compare the objects and the comments of a new database with the scripts (PG* in the environment; scripts/ci.sh)
#
# The objects are the functions, views, tables, operators and operator classes that sql/pg_sift--<default_version>.sql creates in schema sift (a name
# starting with _, an event trigger, a handler and a support function are internal). `Since` is the first install script that creates the name.
# A description is the COMMENT ON of that script, which must also be in the upgrade script into it (a fresh install and an upgrade show the same
# \df+). The released scripts (scripts/released-sql.sha256) cannot get one: their objects take it from docs/reference-notes.tsv instead, which
# also holds the group where the name prefix is not enough and the operator classes created by a DO block. The check fails when
#   - an object has no description, or both a COMMENT and a note, or a note for an object of a script that is not released yet,
#   - a note or a COMMENT names an object that does not exist, an install script and an upgrade script disagree on a COMMENT,
#   - docs/REFERENCE.md or the README block is not what is generated.
set -euo pipefail

cd "$(dirname "$0")/.."
mode=${1:-write}
[[ $mode == write || $mode == --check || $mode == --live ]] || { echo "usage: $0 [--check|--live]" >&2; exit 2; }
exec python3 -I - "$mode" <<'PY'
import csv
import fnmatch
import os
import re
import subprocess
import sys

mode = sys.argv[1]
ROOT = os.getcwd()
errors = []

GROUPS = [
    ('search', 'Search and ranking', 'The `sift` index (BM25): the operator, ranking, query helpers, facets and the inspection of the index.'),
    ('col', 'Column index', 'The `sift_col` index: what it stores, and the operator classes that let the planner scan it.'),
    ('table', 'Tables', 'The `sift_table` access method.'),
    ('maint', 'Maintenance and workers', 'Promotion, merging, rewriting and the background workers, of all three.'),
    ('diag', 'Diagnostics and upgrade', 'Session statistics, why a query kept the normal plan, what an upgrade leaves to do.'),
]
GROUP_IDS = [g[0] for g in GROUPS]
KINDS = {'function': ('function', 'functions'), 'view': ('view', 'views'), 'table': ('table', 'tables'), 'opclass': ('operator class', 'operator classes'),
         'operator': ('operator', 'operators')}
USAGE_SETTINGS = 'maintenance-and-operations'


def err(msg):
    errors.append(msg)


def read(path):
    with open(os.path.join(ROOT, path), encoding='utf-8') as f:
        return f.read()


# ---- SQL: statements with the comments gone and every dollar-quoted body a plain string ----
def statements(text):
    out, cur, i, n = [], [], 0, len(text)

    def quoted(q, i):
        j = i + 1
        while True:
            k = text.index(q, j)
            if text.startswith(q * 2, k):
                j = k + 2
                continue
            return k
    while i < n:
        c = text[i]
        if c == '\\' and (i == 0 or text[i - 1] == '\n'):
            i = text.find('\n', i)
            i = n if i < 0 else i
        elif text.startswith('--', i):
            i = text.find('\n', i)
            i = n if i < 0 else i
        elif text.startswith('/*', i):
            i = text.index('*/', i) + 2
            cur.append(' ')
        elif c in '\'"':
            k = quoted(c, i)
            cur.append(text[i:k + 1])
            i = k + 1
        elif c == '$' and re.match(r'\$[A-Za-z_]*\$', text[i:i + 40]):
            tag = re.match(r'\$[A-Za-z_]*\$', text[i:i + 40]).group(0)
            j = text.index(tag, i + len(tag))
            cur.append("'" + text[i + len(tag):j].replace("'", "''") + "'")
            i = j + len(tag)
        elif c == ';':
            s = ''.join(cur).strip()
            if s:
                out.append(s)
            cur = []
            i += 1
        else:
            cur.append(c)
            i += 1
    s = ''.join(cur).strip()
    return out + ([s] if s else [])


def matching(s, i):
    depth = 0
    while i < len(s):
        c = s[i]
        if c == "'":
            i = s.index("'", i + 1)
            while s.startswith("''", i):
                i = s.index("'", i + 2)
        elif c == '(':
            depth += 1
        elif c == ')':
            depth -= 1
            if depth == 0:
                return i
        i += 1
    raise ValueError('unbalanced parentheses')


def split_top(s):
    out, depth, cur, i = [], 0, '', 0
    while i < len(s):
        c = s[i]
        if c == "'":
            j = s.index("'", i + 1)
            while s.startswith("''", j):
                j = s.index("'", j + 2)
            cur += s[i:j + 1]
            i = j + 1
            continue
        depth += (c == '(') - (c == ')')
        if c == ',' and depth == 0:
            out.append(cur.strip())
            cur = ''
        else:
            cur += c
        i += 1
    return out + ([cur.strip()] if cur.strip() else [])


def norm(s):
    return re.sub(r'\s+', ' ', s).strip()


TWO_WORD = {'double precision', 'character varying', 'timestamp with time zone', 'timestamp without time zone', 'time with time zone'}


def param(p):
    mode_, default = 'IN', None
    m = re.match(r'(INOUT|IN|OUT|VARIADIC)\s+', p, re.I)
    if m:
        mode_, p = m.group(1).upper(), p[m.end():]
    m = re.search(r'\s+DEFAULT\s+(.*)$', p, re.I | re.S)
    if m:
        default, p = norm(m.group(1)), p[:m.start()]
    p = norm(p)
    toks = p.split(' ')
    if len(toks) == 1 or p.lower() in TWO_WORD:
        return mode_, None, p, default
    return mode_, toks[0], ' '.join(toks[1:]), default


STOP = r'AS|LANGUAGE|IMMUTABLE|STABLE|VOLATILE|STRICT|PARALLEL|COST|SUPPORT|SET|RETURN|SECURITY|LEAKPROOF|WINDOW|ROWS|CALLED|TRANSFORM'


def function(s, name, open_):
    close = matching(s, open_)
    params = [param(p) for p in split_top(s[open_ + 1:close])]
    ins = [p for p in params if p[0] in ('IN', 'INOUT', 'VARIADIC')]
    outs = [p for p in params if p[0] in ('OUT', 'INOUT')]
    m = re.match(r'\s*RETURNS\s+(.*?)(?=\s+(?:%s)\b|$)' % STOP, s[close + 1:], re.I | re.S)
    ret = norm(m.group(1)) if m else 'void'
    cols = lambda ps: '(' + ', '.join('%s %s' % (p[1], p[2]) for p in ps) + ')'
    tbl = re.match(r'TABLE\s*\((.*)\)$', ret, re.I | re.S)
    if tbl:
        shown = 'setof (' + norm(tbl.group(1)) + ')'
    elif outs and ret.lower() == 'record':
        shown = cols(outs)
    elif outs and ret.lower() == 'setof record':
        shown = 'setof ' + cols(outs)
    else:
        shown = ret
    args = ', '.join(('VARIADIC ' if p[0] == 'VARIADIC' else '') + ((p[1] + ' ') if p[1] else '') + p[2] + ((' DEFAULT ' + p[3]) if p[3] else '')
                     for p in ins)
    first = ins[0][2] if ins else ''
    internal = ret.lower() in ('event_trigger', 'index_am_handler', 'table_am_handler') or first == 'internal'
    return dict(kind='function', sig='sift.%s(%s)' % (name, args), ret=shown, internal=internal, rawret=ret)


def scan(path):
    """-> objects {(kind, name): dict}, comments {(kind, name): text}"""
    objs, comments = {}, {}
    for s in statements(read(path)):
        m = re.match(r'CREATE\s+(?:OR\s+REPLACE\s+)?FUNCTION\s+sift\.(\w+)\s*\(', s, re.I)
        if m:
            o = function(s, m.group(1), m.end() - 1)
            if m.group(1) in [k[1] for k in objs if k[0] == 'function']:
                err('%s: sift.%s is overloaded (scripts/gen-reference.sh keys the reference by name)' % (path, m.group(1)))
            objs[('function', m.group(1))] = o
            continue
        m = re.match(r'CREATE\s+(?:OR\s+REPLACE\s+)?(VIEW|TABLE)\s+sift\.(\w+)', s, re.I)
        if m:
            k = m.group(1).lower()
            objs[(k, m.group(2))] = dict(kind=k, sig='sift.' + m.group(2), ret='-', internal=False)
            continue
        m = re.match(r'CREATE\s+OPERATOR\s+CLASS\s+sift\.(\w+)\s+(DEFAULT\s+)?FOR\s+TYPE\s+(\w+)\s+USING\s+(\w+)', s, re.I)
        if m:
            objs[('opclass', m.group(1))] = dict(kind='opclass', ret='-', internal=False,
                                                 sig='sift.%s FOR TYPE %s USING %s%s' % (m.group(1), m.group(3), m.group(4), ' (default)' if m.group(2) else ''))
            continue
        m = re.match(r'CREATE\s+OPERATOR\s+(?:pg_catalog\.)?(\S+?)\s*\((.*)\)$', s, re.I | re.S)
        if m:
            f = dict((k.upper(), v) for k, v in re.findall(r'(\w+)\s*=\s*([\w.]+)', m.group(2)))
            fn = f['FUNCTION'].replace('sift.', '')
            objs[('operator', m.group(1))] = dict(kind='operator', sig='%s %s %s' % (f['LEFTARG'], m.group(1), f['RIGHTARG']), ret='bool', internal=False, fn=fn)
            continue
        m = re.match(r'COMMENT\s+ON\s+(FUNCTION|VIEW|TABLE|OPERATOR\s+CLASS|OPERATOR)\s+(?:sift\.|pg_catalog\.)?([^\s(]+)[^\']*?\s+IS\s+(\'(?:[^\']|\'\')*\'|NULL)$', s, re.I | re.S)
        if m:
            k = {'operator class': 'opclass'}.get(norm(m.group(1)).lower(), m.group(1).lower())
            if m.group(3) != 'NULL':
                comments[(k, m.group(2))] = m.group(3)[1:-1].replace("''", "'")
    return objs, comments


# ---- the scripts ----
control = read('pg_sift.control')
latest = re.search(r"default_version\s*=\s*'([^']+)'", control).group(1)
vkey = lambda v: tuple(int(x) for x in v.split('.'))
versions = sorted((m.group(1) for f in os.listdir(os.path.join(ROOT, 'sql')) for m in [re.fullmatch(r'pg_sift--(\d+(?:\.\d+)*)\.sql', f)] if m), key=vkey)
released = {m.group(1) for line in read('scripts/released-sql.sha256').split('\n') for m in [re.search(r'sql/pg_sift--(\d+(?:\.\d+)*)\.sql$', line)] if m}
if latest not in versions:
    sys.exit('reference: sql/pg_sift--%s.sql (default_version) does not exist' % latest)

since = {}
for v in versions:
    o, _ = scan('sql/pg_sift--%s.sql' % v)
    for k in o:
        since.setdefault(k, v)
objs, comments = scan('sql/pg_sift--%s.sql' % latest)

upgrades = {}
for f in sorted(os.listdir(os.path.join(ROOT, 'sql'))):
    if re.fullmatch(r'pg_sift--[\d.]+--%s\.sql' % re.escape(latest), f):
        upgrades.update(scan('sql/' + f)[1])
before = scan('sql/pg_sift--%s.sql' % [v for v in versions if vkey(v) < vkey(latest)][-1])[1]   # a comment the previous version has already is on an updated install
for k, text in comments.items():
    if upgrades.get(k) != text and before.get(k) != text:
        err('sql/pg_sift--%s.sql: the COMMENT on %s %s is not (the same) in the upgrade script into %s' % (latest, k[0], k[1], latest))
for k in upgrades:
    if k not in comments:
        err('the upgrade script into %s has a COMMENT on %s %s that sql/pg_sift--%s.sql lacks' % (latest, k[0], k[1], latest))
for k in comments:
    if k not in objs:
        err('COMMENT on %s %s: no such object in sql/pg_sift--%s.sql' % (k[0], k[1], latest))

# ---- the notes ----
notes = {}   # name -> (group, since, description, line)
for n, line in enumerate(read('docs/reference-notes.tsv').split('\n'), 1):
    if not line.strip() or line.startswith('#'):
        continue
    f = line.split('\t')
    if len(f) != 4:
        err('docs/reference-notes.tsv:%d: %d columns, not 4 (name, group, since, description)' % (n, len(f)))
        continue
    if f[0] in notes:
        err('docs/reference-notes.tsv:%d: %s is twice' % (n, f[0]))
    if f[1] not in GROUP_IDS + ['', '-']:
        err('docs/reference-notes.tsv:%d: group %s is none of %s, - (internal) or empty' % (n, f[1], ' '.join(GROUP_IDS)))
    notes[f[0]] = (f[1], f[2], f[3].strip(), n)
by_name = {}
for k in objs:
    by_name.setdefault(k[1], []).append(k)
globs = {name for name in notes if '*' in name}
for name, (g, sn, d, n) in notes.items():
    if name not in globs and name not in by_name:
        err('docs/reference-notes.tsv:%d: %s is not an object of sql/pg_sift--%s.sql' % (n, name, latest))
    if name in globs and not (sn and d and g not in ('', '-')):
        err('docs/reference-notes.tsv:%d: %s is a family of operator classes: it needs its group, since and description' % (n, name))

# ---- the reference ----
rows = []   # dict(group, kind, name, sig, ret, since, desc, link)
internal = []
for k, o in sorted(objs.items(), key=lambda kv: kv[0][1]):
    kind, name = k
    note = notes.get(name if kind != 'operator' else name)
    g = note[0] if note and note[0] else ('col' if name.startswith('col_') else 'table' if name.startswith('table_') else 'search')
    if o['internal'] or (note and note[0] == '-'):
        internal.append(name)
        continue
    desc = comments.get(k)
    if desc is not None and note and note[2]:
        err('docs/reference-notes.tsv:%d: %s has a COMMENT: remove its description from the notes' % (note[3], name))
    if desc is None:
        if not (note and note[2]):
            err('%s sift.%s has no description: add COMMENT ON to sql/pg_sift--%s.sql and to the upgrade script into it (docs/reference-notes.tsv is for the released scripts)' % (kind, name, latest))
        elif since[k] not in released:
            err('docs/reference-notes.tsv:%d: %s is new in %s, which is not released: use COMMENT ON' % (note[3], name, since[k]))
        else:
            desc = note[2]
    if desc is not None and len(norm(desc)) > 400:
        err('the description of %s is %d characters: keep it to a sentence or two (400)' % (name, len(norm(desc))))
    ret = o['ret']
    if kind == 'operator':
        ret = objs.get(('function', o['fn']), {}).get('rawret', 'bool')
    rows.append(dict(group=g, kind=kind, name=name, sig=o['sig'], ret=ret, since=since[k], desc=norm(desc or ''), link=None))
for name in sorted(globs):
    g, sn, d, _ = notes[name]
    if g != '-':
        rows.append(dict(group=g, kind='opclass', name=name, sig='sift.' + name, ret='-', since=sn, desc=norm(d), link=None))

# ---- links to docs/USAGE.md: the ## section that mentions the name most ----
usage = read('docs/USAGE.md')
sections, fence = [], False
for line in usage.split('\n'):
    if line.startswith('```'):
        fence = not fence
    m = re.match(r'## (.*)$', line) if not fence else None
    if m:
        sections.append([m.group(1).strip(), []])
    elif sections:
        sections[-1][1].append(line)


def slug(title):
    return re.sub(r'\s', '-', re.sub(r'[^\w\s-]', '', title.replace('`', '').lower()))


if USAGE_SETTINGS not in [slug(t) for t, _ in sections]:
    err('docs/USAGE.md has no section %s (the settings table that the reference points to)' % USAGE_SETTINGS)
for r in rows:
    if '*' in r['name']:
        continue
    pat = re.escape(r['name']) if r['kind'] == 'operator' else r'\bsift\.%s\b' % re.escape(r['name'])
    best = max(((len(re.findall(pat, '\n'.join(b))), -i) for i, (t, b) in enumerate(sections)), default=(0, 0))
    if best[0]:
        t = sections[-best[1]][0]
        r['link'] = '[%s](USAGE.md#%s)' % (t.replace('`', ''), slug(t))


def esc(s):
    return s.replace('|', '\\|')


def generate():
    n = {k: sum(1 for r in rows if r['kind'] == k and '*' not in r['name']) for k in KINDS}
    counts = ', '.join('%d %s' % (v, KINDS[k][v != 1]) for k, v in n.items() if v)
    doc = ['# SQL reference', '',
           'Generated by `scripts/gen-reference.sh` from `sql/pg_sift--%s.sql`; do not edit by hand. Descriptions are the `COMMENT ON` of that script (`\\df+`),' % latest,
           'or `docs/reference-notes.tsv` for the objects of the released scripts. `scripts/gen-reference.sh --check` fails when this file is stale or an object has no description.', '',
           'Schema `sift`: %s. The first column is the signature with the defaults; Since is the first release that has the name; the section is the one of [USAGE.md](USAGE.md) that uses it most.' % counts, '',
           'Settings (`sift.*`) are in the table of [Maintenance and operations](USAGE.md#%s); the fixed limits are in [LIMITS.md](LIMITS.md).' % USAGE_SETTINGS, '']
    for gid, title, intro in GROUPS:
        rs = [r for r in rows if r['group'] == gid]
        if not rs:
            continue
        doc += ['## ' + title, '', intro, '', '| Object | Kind | Returns | Since | Description |', '|---|---|---|---|---|']
        for r in sorted(rs, key=lambda r: (r['kind'] == 'opclass', r['name'])):
            d = esc(r['desc']) + (' See [%s](%s).' % (r['link'][1:r['link'].index(']')], r['link'][r['link'].index('(') + 1:-1]) if r['link'] else '')
            doc.append('| `%s` | %s | %s | %s | %s |' % (esc(r['sig']), KINDS[r['kind']][0], '`%s`' % esc(r['ret']) if r['ret'] != '-' else '', r['since'], d))
        doc.append('')
    doc += ['## Internal', '', 'Not meant to be called: ' + ', '.join('`sift.%s`' % x for x in sorted(internal)) + '. Names starting with `_` are for the tests.', '']
    ref = '\n'.join(doc)

    rd = ['<!-- sift-reference:start -->', '## SQL reference', '',
          'Schema `sift` has %s. Signatures, results, the release that added each and a line on what it does are in [docs/REFERENCE.md](docs/REFERENCE.md), generated from the SQL scripts.' % counts, '']
    for gid, title, _ in GROUPS:
        names = sorted(r['name'] for r in rows if r['group'] == gid)
        if names:
            anchor = re.sub(r'\s', '-', title.lower())
            rd.append('- **[%s](docs/REFERENCE.md#%s)**: %s' % (title, anchor, ', '.join('`%s`' % x for x in names)))
    rd += ['', 'Settings (`sift.*`) are in the [settings table](docs/USAGE.md#%s), fixed limits in [docs/LIMITS.md](docs/LIMITS.md).' % USAGE_SETTINGS,
           '<!-- sift-reference:end -->']
    return ref, '\n'.join(rd)


ref, block = generate()
readme = read('README.md')
m = re.search(r'<!-- sift-reference:start -->.*?<!-- sift-reference:end -->', readme, re.S)
if not m:
    err('README.md has no <!-- sift-reference:start --> ... <!-- sift-reference:end --> block')
    new_readme = readme
else:
    new_readme = readme[:m.start()] + block + readme[m.end():]


# ---- --live: the scripts against a new database ----
def live():
    db = 'sift_reference_%d' % os.getpid()
    psql = lambda d, q: subprocess.run(['psql', '-X', '-q', '--csv', '-v', 'ON_ERROR_STOP=1', '-d', d, '-c', q], capture_output=True, text=True)
    r = psql('postgres', 'CREATE DATABASE ' + db)
    if r.returncode:
        err('live: cannot create a database: ' + r.stderr.strip())
        return
    try:
        r = psql(db, 'CREATE EXTENSION pg_sift')
        if r.returncode:
            err('live: CREATE EXTENSION pg_sift: ' + r.stderr.strip())
            return
        r = psql(db, r"""
            SELECT 'function', p.proname, coalesce(obj_description(p.oid, 'pg_proc'), '') FROM pg_proc p JOIN pg_namespace n ON n.oid = p.pronamespace WHERE n.nspname = 'sift'
            UNION ALL SELECT CASE c.relkind WHEN 'v' THEN 'view' ELSE 'table' END, c.relname, coalesce(obj_description(c.oid, 'pg_class'), '')
              FROM pg_class c JOIN pg_namespace n ON n.oid = c.relnamespace WHERE n.nspname = 'sift' AND c.relkind IN ('r', 'p', 'v', 'm')
            UNION ALL SELECT 'opclass', o.opcname, coalesce(obj_description(o.oid, 'pg_opclass'), '') FROM pg_opclass o JOIN pg_namespace n ON n.oid = o.opcnamespace WHERE n.nspname = 'sift'
            UNION ALL SELECT 'operator', o.oprname, coalesce(obj_description(o.oid, 'pg_operator'), '') FROM pg_operator o JOIN pg_proc p ON p.oid = o.oprcode JOIN pg_namespace n ON n.oid = p.pronamespace WHERE n.nspname = 'sift'""")
        if r.returncode:
            err('live: ' + r.stderr.strip())
            return
        seen = set()
        for kind, name, desc in list(csv.reader(r.stdout.splitlines(True)))[1:]:
            if name.startswith('_'):
                continue
            seen.add((kind, name))
            if (kind, name) in objs:
                if (kind, name) in comments and desc.strip() != comments[(kind, name)].strip():
                    err('live: the COMMENT on %s %s in the database differs from the script' % (kind, name))
            elif not (kind == 'opclass' and any(fnmatch.fnmatch(name, g) for g in globs)):
                err('live: %s %s is in the database but not in sql/pg_sift--%s.sql as scripts/gen-reference.sh reads it (a DO block or a statement it does not parse: add the name to docs/reference-notes.tsv)' % (kind, name, latest))
        for k in objs:
            if k not in seen:
                err('live: %s %s is in sql/pg_sift--%s.sql but not in the database' % (k[0], k[1], latest))
        for g in globs:
            if not any(k[0] == 'opclass' and fnmatch.fnmatch(k[1], g) for k in seen):
                err('live: no operator class matches %s of docs/reference-notes.tsv' % g)
    finally:
        psql('postgres', 'DROP DATABASE IF EXISTS %s WITH (FORCE)' % db)


if mode == '--live':
    live()
elif mode == '--check':
    have = read('docs/REFERENCE.md') if os.path.exists(os.path.join(ROOT, 'docs/REFERENCE.md')) else None
    if have != ref + '\n':
        err('docs/REFERENCE.md is not what the scripts generate: run scripts/gen-reference.sh (make reference)')
    if new_readme != readme:
        err('the sift-reference block of README.md is not what the scripts generate: run scripts/gen-reference.sh (make reference)')
elif not errors:
    with open(os.path.join(ROOT, 'docs/REFERENCE.md'), 'w', encoding='utf-8') as f:
        f.write(ref + '\n')
    with open(os.path.join(ROOT, 'README.md'), 'w', encoding='utf-8') as f:
        f.write(new_readme)
if errors:
    for e in errors:
        print('reference: ' + e, file=sys.stderr)
    sys.exit(1)
print('reference: ok, %d objects listed, %d internal' % (len(rows), len(internal)))
PY
