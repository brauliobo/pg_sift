#!/usr/bin/env bash
# The SQL reference: every user-facing object of schema sift, read from the install scripts. docs/REFERENCE.md is generated from them.
# Needs python3 and no PostgreSQL (a second or less); --live asks a server.
#
#   scripts/gen-reference.sh           check, then write docs/REFERENCE.md
#   scripts/gen-reference.sh --check   check, and that docs/REFERENCE.md is the generated one (lint, scripts/ci.sh)
#   scripts/gen-reference.sh --live    compare the objects and the comments of a new database with the scripts (PG* in the environment; scripts/ci.sh)
#
# The objects are the functions, views, tables, operators and operator classes that sql/pg_sift--<default_version>.sql creates in schema sift (a name
# starting with _, an event trigger, a handler, an @internal function and a support function are internal). `Since` is the first install script that creates the name.
# A description is the COMMENT ON of that script, which must also be in the upgrade script into it (a fresh install and an upgrade show the same
# \df+). The released scripts (scripts/released-sql.sha256) cannot get one: their objects take it from docs/reference-notes.tsv instead, which
# also holds the group where the name prefix is not enough and the operator classes created by a DO block. The check fails when
#   - an object has no description, or both a COMMENT and a note, or a note for an object of a script that is not released yet,
#   - a note or a COMMENT names an object that does not exist, an install script and an upgrade script disagree on a COMMENT,
#   - docs/REFERENCE.md is not what is generated.
set -euo pipefail

cd "$(dirname "$0")/.."
mode=${1:-write}
scripts/sql-parts.sh build
[[ $mode == write || $mode == --check || $mode == --live ]] || { echo "usage: $0 [--check|--live]" >&2; exit 2; }
exec python3 -I - "$mode" <<'PY'
import csv
import fnmatch
import os
import re
import subprocess
import sys
sys.dont_write_bytecode = True
sys.path.insert(0, 'scripts')
from genlib import err, errors, esc, exit_on_errors, norm, read

mode = sys.argv[1]
ROOT = os.getcwd()

GROUPS = [
    ('search', 'Search and ranking', 'The `sift` index (BM25): the operator, ranking, query helpers, facets and inspection.'),
    ('col', 'Column index and scopes', 'The `sift_col` index: Format Major 2 inspection, scopes, projections, and operator classes.'),
    ('table', 'Tables', 'The `sift_table` table access method.'),
    ('maint', 'Maintenance and monitoring', 'Polymorphic promotion, merging, maintenance, and monitoring views across all access methods.'),
    ('diag', 'Diagnostics and upgrade', 'Session statistics, refusal diagnostics, and upgrade preflight checks.'),
]
GROUP_KEYS = [g[0] for g in GROUPS]
KINDS = {'function': ('function', 'functions'), 'view': ('view', 'views'), 'table': ('table', 'tables'), 'opclass': ('operator class', 'operator classes'),
         'operator': ('operator', 'operators')}
GUIDE_DOCS = ['docs/ANALYTICS.md', 'docs/SCOPES.md', 'docs/TABLES.md', 'docs/SEARCH.md', 'docs/OPERATIONS.md', 'docs/UPGRADING.md']


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
            end = text.find('\n', i)
            end = n if end < 0 else end
            if '@internal' in text[i:end]:
                cur.append(' /*@internal*/ ')
            i = end
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
    internal = ret.lower() in ('event_trigger', 'index_am_handler', 'table_am_handler') or 'internal' in [p[2] for p in ins]
    return dict(kind='function', sig='sift.%s(%s)' % (name, args), ret=shown, internal=internal, rawret=ret, fname=name, types=','.join(p[2] for p in ins))


first_types = {}   # function name -> the argument types of the first CREATE the scans met: the order of the scans is the order of the releases


def unwrapped(stmts):
    """the statements, and the operators and operator classes that a DO block creates when they are not there yet (the parts of a release can run again)"""
    for s in stmts:
        yield s
        if re.match(r"DO\s+'", s):
            for m in re.finditer(r'\bTHEN\s+(CREATE\s+OPERATOR\b[^;]*);', s, re.I):
                yield m.group(1).strip()


def scan(*paths):
    """-> objects {(kind, name): dict}, comments {(kind, name): text}, of the statements of the scripts in order"""
    objs, comments = {}, {}
    for s in [s for path in paths for s in unwrapped(statements(read(path)))]:
        has_internal_tag = '/*@internal*/' in s
        s_clean = s.replace('/*@internal*/', '').strip()
        m = re.match(r'DROP\s+FUNCTION\s+(?:IF\s+EXISTS\s+)?sift\.(\w+)\s*\(', s_clean, re.I)
        if m:
            objs.pop(('function', m.group(1)), None)    # dropped to be created again with another result
            first_types.pop(m.group(1), None)
            continue
        m = re.match(r'ALTER\s+FUNCTION\s+sift\.(\w+)\s*\(.*?\)\s+SET\s+SCHEMA\s+sift_internal\b', s_clean, re.I | re.S)
        if m:
            objs.pop(('function', m.group(1)), None)
            continue
        m = re.match(r'CREATE\s+(?:OR\s+REPLACE\s+)?FUNCTION\s+sift\.(\w+)\s*\(', s_clean, re.I)
        if m:
            o = function(s_clean, m.group(1), m.end() - 1)
            if has_internal_tag:
                o['internal'] = True
            key = ('function', m.group(1))
            if first_types.setdefault(m.group(1), o['types']) != o['types']:
                key = ('function', '%s(%s)' % (m.group(1), o['types']))   # an overload of the first one the scripts create: its own row, the plain name for the links and the groups
            objs[key] = o
            continue
        s = s_clean
        m = re.match(r'CREATE\s+(?:OR\s+REPLACE\s+)?AGGREGATE\s+sift\.(\w+)\s*\(', s, re.I)
        if m:
            close = matching(s, m.end() - 1)
            fin = re.search(r'FINALFUNC\s*=\s*sift\.(\w+)', s, re.I)
            ret = objs[('function', fin.group(1))]['rawret'] if fin and ('function', fin.group(1)) in objs else '-'
            objs[('function', m.group(1))] = dict(kind='function', sig='sift.%s(%s)' % (m.group(1), norm(s[m.end():close])), ret=ret, internal=False, rawret=ret)
            continue
        m = re.match(r'CREATE\s+(?:OR\s+REPLACE\s+)?(VIEW|TABLE)\s+(?:IF\s+NOT\s+EXISTS\s+)?sift\.(\w+)', s, re.I)
        if m:
            k = m.group(1).lower()
            objs[(k, m.group(2))] = dict(kind=k, sig='sift.' + m.group(2), ret='-', internal=False)
            continue
        m = re.match(r'CREATE\s+OPERATOR\s+CLASS\s+sift\.(\w+)\s+(DEFAULT\s+)?FOR\s+TYPE\s+(\w+)\s+USING\s+(\w+)', s, re.I)
        if m:
            objs[('opclass', m.group(1))] = dict(kind='opclass', ret='-', internal=False,
                                                 sig='sift.%s FOR TYPE %s USING %s%s' % (m.group(1), m.group(3), m.group(4), ' (default)' if m.group(2) else ''))
            continue
        m = re.match(r'CREATE\s+OPERATOR\s+(?:pg_catalog\.|sift\.)?(\S+?)\s*\((.*)\)$', s, re.I | re.S)
        if m:
            f = dict((k.upper(), v) for k, v in re.findall(r'(\w+)\s*=\s*([\w.]+)', m.group(2)))
            fn = f['FUNCTION'].replace('sift.', '')
            types = '%s,%s' % (f['LEFTARG'], f['RIGHTARG'])
            key = ('operator', m.group(1))
            o = dict(kind='operator', sig='%s %s %s' % (f['LEFTARG'], m.group(1), f['RIGHTARG']), ret='bool', internal=False, fn=fn)
            if first_types.setdefault(m.group(1), types) != types:
                key, o['fname'] = ('operator', '%s(%s)' % (m.group(1), types)), m.group(1)
            objs[key] = o
            continue
        m = re.match(r'COMMENT\s+ON\s+(FUNCTION|AGGREGATE|VIEW|TABLE|OPERATOR\s+CLASS|OPERATOR)\s+(?:sift\.|pg_catalog\.)?([^\s(]+)\s*(\([^)]*\))?[^\']*?\s+IS\s+(\'(?:[^\']|\'\')*\'|NULL)$', s, re.I | re.S)
        if m:
            k = {'operator class': 'opclass', 'aggregate': 'function'}.get(norm(m.group(1)).lower(), m.group(1).lower())
            name = m.group(2)
            if '.' in name:
                continue    # another schema than sift (sift_internal): not in the reference
            types = ','.join(norm(t) for t in split_top(m.group(3)[1:-1])) if m.group(3) else ''
            if (k, '%s(%s)' % (name, types)) in objs:
                name = '%s(%s)' % (name, types)    # the comment of an overload
            if m.group(4) != 'NULL':
                comments[(k, name)] = m.group(4)[1:-1].replace("''", "'")
    return objs, comments


# ---- the scripts ----
control = read('pg_sift.control')
latest = re.search(r"default_version\s*=\s*'([^']+)'", control).group(1)
vkey = lambda v: tuple(int(x) for x in v.split('.'))
versions = sorted((m.group(1) for f in os.listdir(os.path.join(ROOT, 'sql')) for m in [re.fullmatch(r'pg_sift--(\d+(?:\.\d+)*)\.sql', f)] if m), key=vkey)
released = {m.group(1) for line in read('scripts/released-sql.sha256').split('\n') for m in [re.search(r'sql/pg_sift--(\d+(?:\.\d+)*)\.sql$', line)] if m}
updates = sorted((vkey(m.group(2)), vkey(m.group(1)), 'sql/' + f) for f in os.listdir(os.path.join(ROOT, 'sql')) for m in [re.fullmatch(r'pg_sift--(\d+(?:\.\d+)*)--(\d+(?:\.\d+)*)\.sql', f)] if m)


def scripts(v):
    """the scripts that create v: its own, else (as PostgreSQL does for a version without one) the newest install script before it and the updates up to v"""
    if v in versions:
        return ['sql/pg_sift--%s.sql' % v]
    base = [x for x in versions if vkey(x) < vkey(v)][-1]
    return ['sql/pg_sift--%s.sql' % base] + [f for to, frm, f in updates if vkey(base) <= frm and to <= vkey(v)]


if latest not in versions and not any(to == vkey(latest) for to, _, _ in updates):
    sys.exit('reference: neither sql/pg_sift--%s.sql nor an update script into it (default_version) exists' % latest)
# a version that has update scripts into it but no install script of its own (a patch release such as 4.0.3) is a release of the chain too: its objects are `Since` it
chain = sorted(set(versions) | {'.'.join(map(str, to)) for to, _, _ in updates} | {latest}, key=vkey)

since = {}
for v in chain:
    o, _ = scan(*scripts(v))
    for k in o:
        since.setdefault(k, v)
objs, comments = scan(*scripts(latest))

upgrades = {}
for f in sorted(os.listdir(os.path.join(ROOT, 'sql'))):
    if re.fullmatch(r'pg_sift--[\d.]+--%s\.sql' % re.escape(latest), f):
        upgrades.update(scan('sql/' + f)[1])
before = scan(*scripts(chain[chain.index(latest) - 1]))[1]   # a comment the previous version has already is on an updated install
for k, text in comments.items():
    if upgrades.get(k) != text and before.get(k) != text:
        err('sql/pg_sift--%s.sql: the COMMENT on %s %s is not (the same) in the upgrade script into %s' % (latest, k[0], k[1], latest))
for k in upgrades:
    if k not in comments:
        err('the upgrade script into %s has a COMMENT on %s %s that sql/pg_sift--%s.sql lacks' % (latest, k[0], k[1], latest))

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
    if f[1] not in GROUP_KEYS + ['', '-']:
        err('docs/reference-notes.tsv:%d: group %s is none of %s, - (internal) or empty' % (n, f[1], ' '.join(GROUP_KEYS)))
    notes[f[0]] = (f[1], f[2], f[3].strip(), n)
by_name = {}
for k in objs:
    by_name.setdefault(objs[k].get('fname', k[1]), []).append(k)
globs = {name for name in notes if '*' in name}
for k in comments:
    if k not in objs and not (k[0] == 'opclass' and any(fnmatch.fnmatch(k[1], g) for g in globs)):   # a DO block creates the operator classes of the families of the notes
        err('COMMENT on %s %s: no such object in sql/pg_sift--%s.sql' % (k[0], k[1], latest))
for name, (g, sn, d, n) in notes.items():
    if name not in globs and name not in by_name:
        err('docs/reference-notes.tsv:%d: %s is not an object of sql/pg_sift--%s.sql' % (n, name, latest))
    if name in globs and not (sn and d and g not in ('', '-')):
        err('docs/reference-notes.tsv:%d: %s is a family of operator classes: it needs its group, since and description' % (n, name))

# ---- the reference ----
rows = []   # dict(group, kind, name, sig, ret, since, desc, link)
internal = []
for k, o in sorted(objs.items(), key=lambda kv: kv[0][1]):
    kind, name = k[0], objs[k].get('fname', k[1])
    note = notes.get(k[1])
    g = note[0] if note and note[0] else ('col' if name.startswith(('col_', 'projection', 'scope_')) else 'table' if name.startswith('table_') else 'search')
    desc = comments.get(k)
    if o['internal'] or name.startswith('_') or (note and note[0] == '-') or (desc or '').startswith('Internal:'):
        internal.append(name)
        continue
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
    first = since[k]
    if note and note[1] and '*' not in name:
        if note[1] not in chain or vkey(note[1]) >= vkey(first):
            err('docs/reference-notes.tsv:%d: %s is Since %s in the scripts: a Since in the notes must be an older version (the release that had it before its part moved to a later script)' % (note[3], name, first))
        first = note[1]
    rows.append(dict(group=g, kind=kind, name=name, sig=o['sig'], ret=ret, since=first, desc=norm(desc or ''), link=None))
for name in sorted(globs):
    g, sn, d, _ = notes[name]
    if g != '-':
        rows.append(dict(group=g, kind='opclass', name=name, sig='sift.' + name, ret='-', since=sn, desc=norm(d), link=None))

# ---- links to the 6 guide docs: the ## section that mentions the name most ----
sections = []
for gpath in GUIDE_DOCS:
    if not os.path.exists(os.path.join(ROOT, gpath)):
        continue
    gbase = os.path.basename(gpath)
    fence = False
    for line in read(gpath).split('\n'):
        if line.startswith('```'):
            fence = not fence
        m = re.match(r'## (.*)$', line) if not fence else None
        if m:
            sections.append([gbase, m.group(1).strip(), []])
        elif sections and sections[-1][0] == gbase:
            sections[-1][2].append(line)


def slug(title):
    return re.sub(r'\s', '-', re.sub(r'[^\w\s-]', '', title.replace('`', '').lower()))


for r in rows:
    if '*' in r['name']:
        continue
    pat = re.escape(r['name']) if r['kind'] == 'operator' else r'\bsift\.%s\b' % re.escape(r['name'])
    best = max(((len(re.findall(pat, '\n'.join(b))), -i) for i, (f, t, b) in enumerate(sections)), default=(0, 0))
    if best[0]:
        f, t, _ = sections[-best[1]]
        r['link'] = '[%s](%s#%s)' % (f[:-3], f, slug(t))


def generate():
    n = {k: sum(1 for r in rows if r['kind'] == k and '*' not in r['name']) for k in KINDS}
    counts = ', '.join('%d %s' % (v, KINDS[k][v != 1]) for k, v in n.items() if v)
    doc = ['# SQL Reference', '',
           'Generated by `scripts/gen-reference.sh` from `sql/pg_sift--%s.sql`; do not edit by hand.' % latest, '',
           'Schema `sift`: %s. Settings live in [OPTIONS.md#server-settings](OPTIONS.md#server-settings) and limits in [LIMITS.md](LIMITS.md).' % counts, '']
    for gid, title, intro in GROUPS:
        rs = [r for r in rows if r['group'] == gid]
        if not rs:
            continue
        doc += ['## ' + title, '', intro, '', '| Object | Returns | Since | Description |', '|---|---|---|---|']
        for r in sorted(rs, key=lambda r: (r['kind'] == 'opclass', r['name'])):
            d = esc(r['desc']) + (' See %s.' % r['link'] if r['link'] else '')
            ret = '*(%s)*' % KINDS[r['kind']][0] if r['kind'] in ('view', 'table', 'opclass') else ('`%s`' % esc(r['ret']) if r['ret'] != '-' else '')
            doc.append('| `%s` | %s | %s | %s |' % (esc(r['sig']), ret, r['since'], d))
        doc.append('')
    doc += ['---', '', '*Internal objects:* Extension internal helpers live in schema `sift_internal` (revoked from `PUBLIC`), and PostgreSQL handler, operator-class, type, and aggregate-support functions in `sift` are tagged `-- @internal` and omitted above.', '']
    return '\n'.join(doc)


ref = generate()


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
        plain = {(k[0], o.get('fname', k[1])) for k, o in objs.items() if not k[1].startswith('_')}   # the names starting with _ are internal: listed in no direction
        texts = {}   # the comments an overloaded name has
        for (kd, nm), t in comments.items():
            texts.setdefault((kd, nm.split('(')[0]), set()).add(t.strip())
        for kind, name, desc in list(csv.reader(r.stdout.splitlines(True)))[1:]:
            if name.startswith('_'):
                continue
            seen.add((kind, name))
            if (kind, name) in plain:
                if (kind, name) in texts and desc.strip() not in texts[(kind, name)]:
                    err('live: the COMMENT on %s %s in the database differs from the script' % (kind, name))
            elif not (kind == 'opclass' and any(fnmatch.fnmatch(name, g) for g in globs)):
                err('live: %s %s is in the database but not in sql/pg_sift--%s.sql as scripts/gen-reference.sh reads it (a DO block or a statement it does not parse: add the name to docs/reference-notes.tsv)' % (kind, name, latest))
        for k in plain:
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
elif not errors:
    with open(os.path.join(ROOT, 'docs/REFERENCE.md'), 'w', encoding='utf-8') as f:
        f.write(ref + '\n')
exit_on_errors('reference: ')
print('reference: ok, %d objects listed, %d internal' % (len(rows), len(internal)))
PY
