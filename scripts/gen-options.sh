#!/usr/bin/env bash
# The options of pg_sift: every reloption of the index kinds and every sift.* setting, read from the C sources. The block of README.md between the
# sift-options markers and the two tables of docs/USAGE.md (sift-settings, sift-reloptions) are generated. Needs python3 and no PostgreSQL (a second
# or less); --live asks a server.
#
#   scripts/gen-options.sh             check, then write README.md and docs/USAGE.md
#   scripts/gen-options.sh --check     check, and that both files are the generated ones (lint, scripts/ci.sh)
#   scripts/gen-options.sh --live      compare the settings (pg_settings) and the reloptions (sift._test_reloptions) of a new database with the sources (PG* in
#                                      the environment, a SIFT_TESTING build; scripts/ci.sh)
#   scripts/gen-options.sh --selftest  prove in memory that a new option without a row, a row without an option and a changed default are caught
#   scripts/gen-options.sh --snapshot  print scripts/options-released.tsv for the current sources (a release replaces the file with it)
#
# What the sources say is read, not copied: the DefineCustom*Variable() calls of src/ give a setting its name, type, default, range, context, unit and
# description; the add_*_reloption() calls give a reloption its name, type, default, range and description. Default and range are the values the library
# is compiled with (a #define is evaluated), so a change of one reaches the tables with `make options`. What a call cannot say (the index kind or object
# a setting is for, what it buys, what it costs, where to read more, the grammar of a string option) is one row of docs/options.tsv per option; settings
# of one family may share a README row (group). The check fails when
#   - an option of src/ has no row, or a row names an option that does not exist (a setting under #ifdef SIFT_TESTING is not documented),
#   - a row lacks its `applies`, `buys` or `cost` (a member of a group takes the first row's), names an unknown index kind, or links to a file or heading that is not there,
#   - a reloption is registered without an entry of the relopt_parse_elt table of its access method, or the reverse,
#   - a default that is not a literal cannot be evaluated (add the PostgreSQL constant to CONST),
#   - README.md or docs/USAGE.md is not what is generated.
# `new in <version>` and `was <default>` in the tables come from scripts/options-released.tsv, the defaults of the last release.
set -euo pipefail

cd "$(dirname "$0")/.."
mode=${1:-write}
[[ $mode == write || $mode == --check || $mode == --live || $mode == --selftest || $mode == --snapshot ]] || { echo "usage: $0 [--check|--live|--selftest|--snapshot]" >&2; exit 2; }
exec python3 -I - "$mode" <<'PY'
import csv
import os
import re
import subprocess
import sys

mode = sys.argv[1]
ROOT = os.getcwd()
CONST = {'INT_MAX': 2147483647, 'INT_MIN': -2147483648, 'MAX_KILOBYTES': 2147483647, 'true': 1, 'false': 0}
KIND_AM = {'sift_relopt_kind': 'sift', 'col_relopt_kind': 'sift_col', 'stub_relopt_kind': None}     # None: the test access method
APPLIES = ['all', 'BM25', 'sift_col', 'sift_table']
CTX = {'PGC_USERSET': 'session', 'PGC_SUSET': 'superuser', 'PGC_SIGHUP': 'reload', 'PGC_POSTMASTER': 'restart'}
ARGS = {'Bool': ['name', 'short', 'long', 'var', 'default', 'ctx', 'flags'],
        'Int': ['name', 'short', 'long', 'var', 'default', 'lo', 'hi', 'ctx', 'flags'],
        'Real': ['name', 'short', 'long', 'var', 'default', 'lo', 'hi', 'ctx', 'flags'],
        'String': ['name', 'short', 'long', 'var', 'default', 'ctx', 'flags'],
        'Enum': ['name', 'short', 'long', 'var', 'default', 'members', 'ctx', 'flags']}
RELARGS = {'int': ['kind', 'name', 'short', 'default', 'lo', 'hi', 'lock'], 'real': ['kind', 'name', 'short', 'default', 'lo', 'hi', 'lock'],
           'bool': ['kind', 'name', 'short', 'default', 'lock'], 'string': ['kind', 'name', 'short', 'default', 'check', 'lock'],
           'enum': ['kind', 'name', 'short', 'members', 'default', 'detail', 'lock']}


def read(path):
    with open(os.path.join(ROOT, path), encoding='utf-8') as f:
        return f.read()


def strip_comments(text):
    def blank(m):
        t = m.group(0)
        return t if t[0] in '"\'' else re.sub(r'[^\n]', ' ', t)
    return re.sub(r'"(?:\\.|[^"\\\n])*"|\'(?:\\.|[^\'\\\n])*\'|/\*.*?\*/|//[^\n]*', blank, text, flags=re.S)


def split_args(s):
    out, depth, cur, i = [], 0, '', 0
    while i < len(s):
        c = s[i]
        if c == '"':
            j = i + 1
            while s[j] != '"':
                j += 2 if s[j] == '\\' else 1
            cur += s[i:j + 1]
            i = j + 1
            continue
        depth += (c in '({') - (c in ')}')
        if c == ',' and depth == 0:
            out.append(cur.strip())
            cur = ''
        else:
            cur += c
        i += 1
    return out + [cur.strip()]


def call_end(text, i):
    depth = 0
    while True:
        c = text[i]
        if c == '"':
            i += 1
            while text[i] != '"':
                i += 2 if text[i] == '\\' else 1
        elif c == '(':
            depth += 1
        elif c == ')':
            depth -= 1
            if depth == 0:
                return i
        i += 1


def string(a):
    if a == 'NULL':
        return ''
    return ''.join(re.sub(r'\\(.)', r'\1', s) for s in re.findall(r'"((?:\\.|[^"\\])*)"', a))


def norm(s):
    return re.sub(r'\s+', ' ', s).strip()


def esc(s):
    return s.replace('|', '\\|')


def testing_lines(text):
    """-> the set of line numbers (1-based) inside #ifdef SIFT_TESTING (its #else branch is outside)"""
    stack, out = [], set()
    for n, line in enumerate(text.split('\n'), 1):
        m = re.match(r'\s*#\s*(ifdef|ifndef|if|elif|else|endif)\b\s*(.*)', line)
        if m:
            d, rest = m.groups()
            if d in ('ifdef', 'ifndef', 'if'):
                stack.append((d == 'ifdef' and rest.strip() == 'SIFT_TESTING') or (d == 'ifndef' and rest.strip() == 'SIFT_TESTING' and 'neg'))
            elif d == 'else' and stack:
                stack[-1] = {True: False, False: False, 'neg': True}[stack[-1]]
            elif d == 'endif' and stack:
                stack.pop()
        if any(stack):
            out.add(n)
    return out


# ---- the sources ----
def collect(srcs):
    """srcs: {path: text} -> (options, errors). An option: dict(key, kind, am, name, type, default, lo, hi, unit, ctx, short, long, members, testing, where)"""
    errors, defs, texts = [], {}, {}
    for path, raw in sorted(srcs.items()):
        texts[path] = strip_comments(raw)
        for m in re.finditer(r'^[ \t]*#[ \t]*define[ \t]+(\w+)[ \t]+(.*(?:\\\n.*)*)$', texts[path], re.M):
            defs.setdefault(m.group(1), []).append((path, m.group(2).replace('\\\n', ' ').strip()))

    def ev(expr, path, depth=0):
        e = re.sub(r'\bUINT64CONST\s*\(', '(', expr)
        e = re.sub(r'\(\s*(?:Size|uint32|uint64|int64|int|unsigned|long|double)\s*\)', '', e)
        e = re.sub(r'\b(0[xX][0-9a-fA-F]+|\d+)[uUlL]+\b', r'\1', e)
        if '.' not in e:
            e = e.replace('/', '//')

        def sub(m):
            name = m.group(0)
            if name in CONST:
                return str(CONST[name])
            pick = [d for d in defs.get(name, []) if d[0] == path] or defs.get(name, [])
            if depth > 10 or not pick or len({d[1] for d in pick}) != 1:
                raise ValueError('cannot evaluate %s (add it to CONST of scripts/gen-options.sh)' % name)
            return '(%s)' % ev(pick[0][1], pick[0][0], depth + 1)
        return eval(re.sub(r'\b[A-Za-z_]\w*\b', sub, e), {'__builtins__': {}}, {})

    def enum_labels(var, path):
        """the labels of a config_enum_entry or relopt_enum_elt_def array: [(label, constant text, hidden)]"""
        m = re.search(r'\b%s\[\]\s*=\s*\{(.*?)\n\s*\};' % re.escape(var), texts[path], re.S)
        return [(g[0], g[1].strip(), g[2].strip() == 'true') for g in re.findall(r'\{\s*"([^"]*)"\s*,\s*([^,}]+?)\s*(?:,\s*(\w+)\s*)?\}', m.group(1))] if m else None

    opts = []
    for path, text in texts.items():
        tl = testing_lines(text)
        calls = [(m.start(), m.end() - 1, 'guc', m.group(1)) for m in re.finditer(r'\bDefineCustom(Bool|Int|Real|String|Enum)Variable\(', text)]
        calls += [(m.start(), m.end() - 1, 'guc', 'Enum') for m in re.finditer(r'\bsift_host_enum_option\(', text) if not text[:m.start()].rstrip().endswith('define')]
        calls += [(m.start(), m.end() - 1, 'rel', m.group(1)) for m in re.finditer(r'\badd_(int|real|bool|string|enum)_reloption\(', text)]
        for start, op, what, t in calls:
            line = text.count('\n', 0, start) + 1
            where = '%s:%d' % (path, line)
            a = split_args(text[op + 1:call_end(text, op)])
            if what == 'guc' and not a[0].startswith('"'):      # the body of the sift_host_enum_option macro
                continue
            try:
                if what == 'guc' and text.startswith('sift_host_enum_option', start):
                    a = dict(zip(['name', 'short', 'long', 'var', 'default', 'members'], a))      # sift_host_enum_option(name, short, long, var, boot, options, assign)
                    a['ctx'], a['flags'] = 'PGC_USERSET', ''
                elif what == 'guc':
                    a = dict(zip(ARGS[t], a))
                else:
                    a = dict(zip(RELARGS[t], a))
                o = dict(kind=what, type=t.lower(), where=where, testing=line in tl or string(a['name']).startswith('sift.test_'), name=string(a['name']), short=norm(string(a['short'])),
                         long=norm(string(a.get('long', 'NULL'))), unit='', ctx=CTX.get(a.get('ctx', ''), ''), lo=None, hi=None, am=None, members=None)
                if what == 'rel':
                    if a['kind'] not in KIND_AM:
                        errors.append('%s: add_%s_reloption(%s, ...): the reloption kind is none of %s (add it to KIND_AM of scripts/gen-options.sh)' % (where, t, a['kind'], ', '.join(KIND_AM)))
                        continue
                    o['am'] = KIND_AM[a['kind']]
                    if o['am'] is None:
                        continue
                    o['key'] = '%s(%s)' % (o['am'], o['name'])
                else:
                    o['key'] = o['name']
                    um = re.search(r'GUC_UNIT_(\w+)', a['flags'])
                    o['unit'] = {'KB': 'kB', 'MB': 'MB', 'MS': 'ms'}.get(um.group(1), um.group(1).lower()) if um else ''
                if o['type'] == 'enum':
                    labels = enum_labels(a['members'], path)
                    if labels is None:
                        errors.append('%s: the labels %s of %s are not an array of this file' % (where, a['members'], o['name']))
                        continue
                    o['members'] = [l for l, _, hidden in labels if not hidden]
                    o['default'] = next((l for l, c, _ in labels if c == a['default']), None)
                    if o['default'] is None:
                        errors.append('%s: the default %s of %s is none of the labels' % (where, a['default'], o['name']))
                elif o['type'] == 'string':
                    o['default'] = string(a['default'])
                elif o['type'] == 'bool':
                    o['default'] = bool(ev(a['default'], path))
                else:
                    o['default'] = ev(a['default'], path)
                    o['lo'], o['hi'] = ev(a['lo'], path), ev(a['hi'], path)
                opts.append(o)
            except (ValueError, KeyError, TypeError, SyntaxError, NameError) as ex:
                errors.append('%s: %s' % (where, ex))
    for p, text in texts.items():       # the table of build_reloptions() of every access method names the options it parses
        for m in re.finditer(r'relopt_parse_elt\s+\w*tab\[\]\s*=\s*\{(.*?)\};', text, re.S):
            am = {'col/am.c': 'sift_col', 'core/handler.c': 'sift'}.get(p)
            parsed = set(re.findall(r'\{\s*"(\w+)"\s*,\s*RELOPT_TYPE', m.group(1)))
            for n in sorted(parsed ^ {o['name'] for o in opts if o['kind'] == 'rel' and o['am'] == am}) if am else ():
                errors.append('src/%s: the reloption %s of %s is %s' % (p, n, am, 'parsed but not registered' if n in parsed else 'registered but not parsed (relopt_parse_elt)'))
    seen = set()
    for o in opts:
        if o['key'] in seen:
            errors.append('%s: %s is defined twice' % (o['where'], o['key']))
        seen.add(o['key'])
    return opts, errors


def num(v):
    return str(int(v)) if v == int(v) else '%g' % v


def fmt(o, v):
    if o['type'] == 'bool':
        return ('true' if v else 'false') if o['kind'] == 'rel' else ('on' if v else 'off')
    if o['type'] == 'string':
        return "'%s'" % v if v else "''"
    if o['type'] == 'real':
        return num(v)
    if o['type'] == 'int':
        return '%d%s' % (v, ' ' + o['unit'] if o['unit'] and v > 0 else '')
    return str(v)


def values(o, row):
    if o['type'] == 'bool':
        return 'bool'
    if o['type'] == 'enum':
        return 'enum: ' + ', '.join(o['members'])
    if o['type'] == 'string':
        return row['values'] or 'text'
    return '%s %s..%s%s' % (o['type'], num(o['lo']), 'max' if o['hi'] == 2147483647 else num(o['hi']), ' ' + o['unit'] if o['unit'] else '')


# ---- the notes ----
def read_tsv(text, cols, name, errors):
    rows = {}
    for n, line in enumerate(text.split('\n'), 1):
        if not line.strip() or line.startswith('#'):
            continue
        f = line.split('\t')
        if len(f) != len(cols):
            errors.append('%s:%d: %d columns, not %d (%s)' % (name, n, len(f), len(cols), ', '.join(cols)))
            continue
        r = dict(zip(cols, [x.strip() for x in f]), line=n)
        if r[cols[0]] in rows:
            errors.append('%s:%d: %s is twice' % (name, n, r[cols[0]]))
        rows[r[cols[0]]] = r
    return rows


NOTE_COLS = ['key', 'applies', 'group', 'values', 'minor', 'buys', 'cost', 'link']


def anchors(path, cache={}):
    """-> {anchor: heading} of a file of docs/"""
    if path not in cache:
        fence, out = False, {}
        for line in read('docs/' + path).split('\n'):
            fence ^= line.startswith('```')
            m = None if fence else re.match(r'#{1,6}\s+(.*?)\s*#*$', line)
            if m:
                out[re.sub(r'\s', '-', re.sub(r'[^\w\s-]', '', m.group(1).replace('`', '').lower()))] = m.group(1).replace('`', '')
        cache[path] = out
    return cache[path]


def check(opts, notes, errors):
    docs = [o for o in opts if not o['testing']]
    keys = {o['key'] for o in docs}
    minor_now = next((o['default'] for o in docs if o['key'] == 'sift.col_format_minor'), 0)
    # an option may stamp the one minor above the default (point_tid_columns: 16, key_cut: 17), which the library reads (SIFT_COL_FORMAT_MAX_READ)
    m = re.search(r'#define SIFT_COL_FORMAT_KEYED\s+(\d+)', open('src/col/col.h').read())
    minor_max = max(int(minor_now), int(m.group(1))) if m else int(minor_now)
    for o in docs:
        if o['key'] not in notes:
            errors.append('%s: %s has no row in docs/options.tsv (applies, buys, cost, link: what it is for, what it costs; `-` in applies hides an alias)' % (o['where'], o['key']))
    group_first = {}
    for k, r in notes.items():
        n = 'docs/options.tsv:%d: %s' % (r['line'], k)
        if k not in keys:
            errors.append('%s is not an option of src/ (or it is under #ifdef SIFT_TESTING)' % n)
            continue
        if r['applies'] == '-':
            if not r['buys']:
                errors.append('%s: applies `-` needs the reason in buys' % n)
            continue
        for a in r['applies'].split(','):
            if a.strip() not in APPLIES:
                errors.append('%s: applies %s is none of %s' % (n, a, ', '.join(APPLIES)))
        first = group_first.setdefault(r['group'], r) if r['group'] else r
        o = next(o for o in docs if o['key'] == k)
        for c in ('buys', 'cost'):
            if not (r[c] or first[c]):
                errors.append('%s: no %s' % (n, c))
        if r['minor'] and not (o['am'] == 'sift_col' and r['minor'].isdigit() and 7 <= int(r['minor']) <= minor_max):
            errors.append('%s: minor %s is not a column format minor of a sift_col option (7 to %s)' % (n, r['minor'], minor_max))
        if o['type'] == 'string' and not r['values']:
            errors.append('%s: a string option needs its values (the grammar in a few words)' % n)
        if r['link']:
            f, _, anchor = r['link'].partition('#')
            if not os.path.exists(os.path.join(ROOT, 'docs', f)) or (anchor and anchor not in anchors(f)):
                errors.append('%s: the link %s is not a file%s of docs/' % (n, r['link'], ' and heading' if anchor else ''))


def release_label():
    return re.search(r"default_version\s*=\s*'(\d+\.\d+)", read('pg_sift.control')).group(1)


def released_label():
    return re.search(r'released: ([\d.]+)', read('scripts/options-released.tsv')).group(1)


def snapshot(opts):
    return '\n'.join('%s\t%s' % (o['key'], fmt(o, o['default'])) for o in sorted(opts, key=lambda o: o['key']) if not o['testing'])


# ---- the tables ----
def link(r, prefix):
    """[COL 2.14](path#heading): the file, and the number of the heading when it has one"""
    if not r['link']:
        return ''
    f, _, a = r['link'].partition('#')
    num_ = re.match(r'\d+(\.\d+)*', anchors(f).get(a, ''))
    label = os.path.splitext(os.path.basename(f))[0] + (' ' + num_.group(0) if num_ else '')
    return '[%s](%s%s)' % (label, prefix, r['link'])


def generate(opts, notes, released, version, old_version):
    by_key = {o['key']: o for o in opts}
    listed = [(by_key[k], r) for k, r in notes.items() if r['applies'] != '-']
    lead = {}
    for o, r in listed:
        lead.setdefault(r['group'] or o['key'], r)
    old_minor = int(released.get('sift.col_format_minor', '0'))

    def meta(r, c):
        return r[c] or lead[r['group'] or r['key']][c]

    def default(o):
        d = fmt(o, o['default'])
        if o['key'] not in released:
            return '%s (new in %s)' % (d, version)
        return d if released[o['key']] == d else '%s (was %s in %s)' % (d, released[o['key']], old_version)

    def cost(r):
        c = meta(r, 'cost')
        if r['minor']:
            c = 'format minor %s%s%s' % (r['minor'], '; a 3.2 library cannot read it' if int(r['minor']) > old_minor else '', '; ' + c if c else '')
        return c

    rows, done = [], set()      # the README: a row per option, a row per group
    for o, r in listed:
        if r['group']:
            if r['group'] in done:
                continue
            done.add(r['group'])
            ms = [(m, mr) for m, mr in listed if mr['group'] == r['group']]
            pre = os.path.commonprefix([m['name'] for m, _ in ms])
            pre = pre[:max(pre.rfind('_'), pre.rfind('.')) + 1]
            rows.append(('`%s{%s}`' % (pre, ','.join(m['name'][len(pre):] for m, _ in ms)), r['applies'], '', ', '.join('%s=%s' % (m['name'][len(pre):], default(m)) for m, _ in ms),
                         r['buys'], cost(r), link(r, 'docs/')))
        else:
            rows.append(('`%s`' % o['name'], r['applies'], values(o, r), default(o), r['buys'], cost(r), link(r, 'docs/')))
    readme = ['<!-- sift-options:start -->', '## Options at a glance', '',
              'Every index option (`WITH (...)`) and every `sift.*` setting, generated by `scripts/gen-options.sh` from the C sources: the defaults are the ones the library is compiled with.',
              'The full tables, with range, context and description, are in [docs/USAGE.md](docs/USAGE.md#maintenance-and-operations).', '',
              '<details>', '<summary>The table: %d options and settings with their defaults, what each buys and what it costs</summary>' % len(rows), '',
              '| Option | Applies to | Type and values | Default | What it buys | Cost | Read more |', '|---|---|---|---|---|---|---|']
    readme += ['| ' + ' | '.join(esc(c) for c in row) + ' |' for row in rows]
    readme += ['', '</details>', '<!-- sift-options:end -->']

    gs = ['| Setting | Applies to | Default | Values | Context | Description | Cost |', '|---|---|---|---|---|---|---|']
    rs = ['| Index option | Index | Default | Values | Description | Cost |', '|---|---|---|---|---|---|']
    for o, r in listed:
        c = (cost(r) + (' ' + link(r, '') if r['link'] else '')).strip()
        d = (o['short'] + ' ' + o['long']).strip()
        if o['kind'] == 'guc':
            gs.append('| `%s` | %s | %s | %s | %s | %s | %s |' % (o['name'], r['applies'], default(o), values(o, r), o['ctx'], esc(d), esc(c)))
        else:
            rs.append('| `%s` | `%s` | %s | %s | %s | %s |' % (o['name'], o['am'], default(o), values(o, r), esc(d), esc(c)))
    return '\n'.join(readme), '\n'.join(['<!-- sift-settings:start -->'] + gs + ['<!-- sift-settings:end -->']), '\n'.join(['<!-- sift-reloptions:start -->'] + rs + ['<!-- sift-reloptions:end -->'])


def splice(text, tag, block, errors, path):
    m = re.search(r'<!-- %s:start -->.*?<!-- %s:end -->' % (tag, tag), text, re.S)
    if not m:
        errors.append('%s has no <!-- %s:start --> ... <!-- %s:end --> block' % (path, tag, tag))
        return text
    return text[:m.start()] + block + text[m.end():]


def sources():
    out = {}
    for d, _, fs in os.walk(os.path.join(ROOT, 'src')):
        for f in fs:
            if f.endswith(('.c', '.h')):
                out[os.path.relpath(os.path.join(d, f), os.path.join(ROOT, 'src'))] = open(os.path.join(d, f), encoding='utf-8').read()
    return out


def load(errors, srcs=None, notes_text=None):
    opts, errs = collect(srcs or sources())
    errors += errs
    notes = read_tsv(notes_text if notes_text is not None else read('docs/options.tsv'), NOTE_COLS, 'docs/options.tsv', errors)
    check(opts, notes, errors)
    rel = read_tsv(read('scripts/options-released.tsv'), ['key', 'default'], 'scripts/options-released.tsv', errors)
    return opts, notes, {k: r['default'] for k, r in rel.items()}


def render(opts, notes, released):
    return generate(opts, notes, released, release_label(), released_label())


# ---- --live ----
def live(opts):
    errors = []
    db = 'sift_options_%d' % os.getpid()
    psql = lambda d, q: subprocess.run(['psql', '-X', '-q', '--csv', '-v', 'ON_ERROR_STOP=1', '-d', d], input=q, capture_output=True, text=True)
    r = psql('postgres', 'CREATE DATABASE ' + db)
    if r.returncode:
        return ['live: cannot create a database: ' + r.stderr.strip()]
    try:
        # one session: the settings of the library exist once it is loaded
        r = psql(db, """CREATE EXTENSION pg_sift; CREATE EXTENSION pg_sift_test; SELECT sift.version() \\g /dev/null
            SELECT 'guc', name, vartype, boot_val, coalesce(min_val, ''), coalesce(max_val, ''), context, coalesce(unit, ''), short_desc, coalesce(extra_desc, ''), coalesce(array_to_string(enumvals, ','), '')
              FROM pg_settings WHERE name LIKE 'sift.%';
            SELECT 'rel', am, name, type, dflt, '', '', '', '', '', '' FROM sift._test_reloptions()""")
        if r.returncode:
            return ['live: ' + r.stderr.strip() + ' (a SIFT_TESTING build is needed: scripts/pgtest.sh install)']
        out = [x for x in csv.reader(r.stdout.splitlines(True)) if len(x) == 11 and x[0] in ('guc', 'rel')]
        have = {}
        for x in out:
            if x[0] == 'guc':
                have[x[1]] = x
            else:
                have['%s(%s)' % (x[1], x[2])] = x
        mine = {o['key']: o for o in opts}
        ctx = {'user': 'session', 'superuser': 'superuser', 'sighup': 'reload', 'postmaster': 'restart'}
        for k, x in sorted(have.items()):
            o = mine.get(k)
            if o is None:
                errors.append('live: %s is in the library but not in src/ as scripts/gen-options.sh reads it (a call it does not parse)' % k)
                continue
            if o['kind'] == 'guc':
                _, _, vt, boot, lo, hi, cx, unit, short, extra, enums = x
                same = {'bool': lambda: (boot == 'on') == o['default'], 'integer': lambda: int(boot) == o['default'] and int(lo) == o['lo'] and int(hi) == o['hi'],
                        'real': lambda: float(boot) == o['default'] and float(lo) == o['lo'] and float(hi) == o['hi'], 'string': lambda: boot == o['default'],
                        # sift.simd starts at the top level of the architecture (src/core/cpu.h): neon on AArch64, which hides the levels of x86-64 (avx2, avx512) that src/ lists
                        'enum': lambda: (boot == o['default'] or (k == 'sift.simd' and boot == 'neon' and os.uname().machine in ('aarch64', 'arm64')))
                                        and set(o['members']) - ({'avx2', 'avx512'} if k == 'sift.simd' and os.uname().machine in ('aarch64', 'arm64') else set()) <= set(enums.split(','))}[vt]
                if not same():
                    errors.append('live: %s: the library has default %s (%s%s) but src/ says %s' % (k, boot, lo, '..' + hi if hi else '', fmt(o, o['default'])))
                if ctx.get(cx) != o['ctx']:
                    errors.append('live: %s: the context is %s in the library, %s in src/' % (k, cx, o['ctx']))
                if norm(short) != o['short'] or norm(extra) != o['long']:
                    errors.append('live: %s: the description of the library differs from src/' % k)
            else:
                typ, dflt = x[3], x[4]
                if typ != o['type']:
                    errors.append('live: %s: type %s in the library, %s in src/' % (k, typ, o['type']))
                elif typ != 'enum' and {'bool': lambda: (dflt == 'true') == o['default'], 'int': lambda: int(dflt) == o['default'],
                                        'real': lambda: float(dflt) == o['default'], 'string': lambda: dflt == o['default']}[typ]() is False:
                    errors.append('live: %s: the library has default %s but src/ says %s' % (k, dflt, fmt(o, o['default'])))
        for k, o in mine.items():
            if k not in have and not o['testing']:
                errors.append('live: %s is in src/ but not in the library' % k)
    finally:
        psql('postgres', 'DROP DATABASE IF EXISTS %s WITH (FORCE)' % db)
    return errors


# ---- --selftest: the checks fail on what they are for ----
def selftest():
    srcs = sources()
    errs = []
    opts, notes, released = load(errs)
    if errs:
        return errs
    text = read('docs/options.tsv')
    victim = next(o for o in opts if o['kind'] == 'guc' and not o['testing'] and not notes[o['key']]['group'])
    victim_row = next(l for l in text.split('\n') if l.startswith(victim['key'] + '\t'))
    cases = [
        ('a row is missing', dict(notes_text=text.replace(victim_row + '\n', '')), victim['key'] + ' has no row'),
        ('a row names no option', dict(notes_text=text + 'sift.no_such_setting\tall\t\t\t\tx\ty\t\n'), 'sift.no_such_setting is not an option'),
        ('a new setting', dict(srcs={**srcs, 'core/handler.c': srcs['core/handler.c'] + '\nvoid x(void){ DefineCustomBoolVariable("sift.zz_new", "s", NULL, &v, true, PGC_USERSET, 0, NULL, NULL, NULL); }'}),
         'sift.zz_new has no row'),
        ('a new reloption', dict(srcs={**srcs, 'col/am.c': srcs['col/am.c'] + '\nvoid x(void){ add_bool_reloption(col_relopt_kind, "zz_new", "d", true, ShareUpdateExclusiveLock); }'}),
         'sift_col(zz_new) has no row'),
        ('a reloption that is not parsed', dict(srcs={**srcs, 'col/am.c': srcs['col/am.c'].replace('{"superunit", RELOPT_TYPE_INT, offsetof(SiftColOptions, superunit)},', '')}),
         'superunit of sift_col is registered but not parsed'),
        ('a link to nowhere', dict(notes_text=text.replace(victim_row, victim_row.rsplit('\t', 1)[0] + '\tNOWHERE.md#x')), 'NOWHERE.md#x is not a file'),
    ]
    for what, kw, want in cases:
        e = []
        load(e, **kw)
        if not any(want in x for x in e):
            errs.append('selftest: %s was not caught (wanted "%s", got %s)' % (what, want, e[:2]))
    # a changed default reaches the tables and is flagged against the release
    flip = next(o for o in opts if o['kind'] == 'guc' and o['type'] == 'bool' and not o['testing'] and o['key'] in released)
    changed = srcs[flip['where'].split(':')[0]]
    pat = re.compile(r'(DefineCustomBoolVariable\(\s*"%s".*?&\w+,\s*)(true|false)' % re.escape(flip['name']), re.S)
    new = pat.sub(lambda m: m.group(1) + ('false' if m.group(2) == 'true' else 'true'), changed, count=1)
    e = []
    o2, n2, r2 = load(e, srcs={**srcs, flip['where'].split(':')[0]: new})
    a, b = render(opts, notes, released), render(o2, n2, r2)
    if e or a == b or ('was ' + fmt(flip, flip['default'])) not in b[1]:
        errs.append('selftest: a changed default of %s is not in the tables or not marked "was" (%s)' % (flip['name'], e[:1]))
    return errs


errors = []
if mode == '--selftest':
    errors = selftest()
    if not errors:
        print('options: selftest ok')
elif mode == '--snapshot':
    opts, errs = collect(sources())
    errors += errs
    if not errors:
        print('# released: %s, the defaults of the last release (a release replaces this file with scripts/gen-options.sh --snapshot)\n%s' % (release_label(), snapshot(opts)))
else:
    opts, notes, released = load(errors)
    if mode == '--live':
        errors += live(opts)
        if not errors:
            print('options: live ok, the library agrees with src/ on every setting and reloption')
    elif not errors:
        readme, settings, reloptions = render(opts, notes, released)
        files = {'README.md': [('sift-options', readme)], 'docs/USAGE.md': [('sift-settings', settings), ('sift-reloptions', reloptions)]}
        for path, blocks in files.items():
            old = new = read(path)
            for tag, block in blocks:
                new = splice(new, tag, block, errors, path)
            if mode == '--check' and new != old:
                errors.append('%s is not what the sources generate: run scripts/gen-options.sh (make options)' % path)
            elif mode == 'write' and not errors:
                with open(os.path.join(ROOT, path), 'w', encoding='utf-8') as f:
                    f.write(new)
        if not errors:
            n = [o for o in opts if not o['testing']]
            print('options: ok, %d settings and %d reloptions listed' % (sum(o['kind'] == 'guc' for o in n), sum(o['kind'] == 'rel' for o in n)))
if errors:
    for e in errors:
        print('options: ' + e, file=sys.stderr)
    sys.exit(1)
PY
