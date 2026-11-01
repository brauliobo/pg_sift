# The tables generated from the options: release labels, notices, the --missing list, --snapshot and the README, USAGE and OPTIONS blocks.
# Run into the namespace of main.py (exec), not imported; not a script of its own.
def release_label():
    return re.search(r"default_version\s*=\s*'(\d+\.\d+)", read('pg_sift.control')).group(1)


def notices(opts):
    """-> [(level, message, source file, [the options it names])] of every NOTICE and WARNING the library raises (a message of #ifdef SIFT_TESTING code or of a test-only file is not listed)"""
    names = sorted({o['name'] for o in opts if not o['testing'] and o['kind'] in ('guc', 'rel')}, key=len, reverse=True)
    out = []
    for path, text in sorted(sources().items()):
        if re.search(r'(^|/)(test\.c|[^/]*_test[^/]*\.[ch])$', path):
            continue
        t = strip_comments(text)
        skip = testing_lines(text)
        for m in re.finditer(r'\b(?:ereport|elog)\(\s*(NOTICE|WARNING)\b', t):
            if t.count('\n', 0, m.start()) + 1 in skip:
                continue
            i = t.index('(', m.start())
            body = t[i + 1:call_end(t, i)]
            e = re.search(r'\berrmsg(?:_internal)?\s*\(', body)
            if e:
                j = body.index('(', e.start())
                msg = string(split_args(body[j + 1:call_end(body, j)])[0])
            else:
                msg = string(split_args(body)[1])
            if msg:
                out.append((m.group(1), msg, path, [n for n in names if re.search(r'(?<![\w])%s(?![\w])' % re.escape(n), msg)]))
    return out


def lead_rows(notes):
    """-> {group or key: the first row of the group}: the cells a member leaves empty are its"""
    lead = {}
    for k, r in notes.items():
        lead.setdefault(r['group'] or k, r)
    return lead


def missing(notes):
    """-> [(key, [cells])]: the measurements a default or an opt-in has not got (`not measured`; `-` says there is no such effect)"""
    lead = lead_rows(notes)
    out = []
    for k, r in notes.items():
        cell = lambda c: r[c] or lead[r['group'] or k][c]
        if cell('class') in ('default', 'opt-in'):
            m = [c for c in EVIDENCE if cell(c) in ('', 'not measured')] + (['bench'] if cell('gain') != '-' and cell('bench') in ('', '-') else []) + (['why'] if cell('why') == 'unknown' else [])
            m += [] if cell('class') == 'default' and not (r['minor'] or lead[r['group'] or k]['minor']) else [c for c in FORMULA if cell(c) == '' and not (c in ('s', 'save') and cell('gain') == '-') and not (c == 'g' and cell('growth') == '-') and not (c == 'w' and cell('write') == '-')]
            if m:
                out.append((k, m))
    return out


def col_max_read():
    """-> the newest column format minor this tree reads (SIFT_COL_FORMAT_MAX_READ, through the macros it names)"""
    h = read('src/col/col.h')
    v = re.search(r'#define SIFT_COL_FORMAT_MAX_READ\s+(\w+)', h).group(1)
    while not v.isdigit():
        v = re.search(r'#define %s\s+(\w+)' % v, h).group(1)
    return int(v)


def released_max_read():
    return int(re.search(r'column format read: (\d+)', read('scripts/options-released.tsv')).group(1))


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
    by_key.update({k: heuristic(k) for k in notes if k.startswith('auto(')})
    listed = [(by_key[k], r) for k, r in notes.items() if r['applies'] != '-' and by_key[k]['kind'] != 'auto']
    lead = lead_rows(notes)
    old_minor = int(released.get('sift.col_format_minor', '0'))

    def meta(r, c):
        return r[c] or lead[r['group'] or r['key']][c]

    def default(o, marks=True):     # marks: `new in` and `was`, for the tables of USAGE.md; the README says what the default is
        d = fmt(o, o['default'])
        if not marks:
            return d
        if o['kind'] == 'auto':
            return d
        if o['key'] not in released:
            return '%s (new in %s)' % (d, notes[o['key']]['since'] or version)
        return d if released[o['key']] == d else '%s (was %s in %s)' % (d, released[o['key']], old_version)

    def cost(r, marks=True):
        c = meta(r, 'cost')
        if r['minor']:
            c = 'format minor %s%s%s' % (r['minor'], '; a 3.2 library cannot read it' if marks and int(r['minor']) > old_minor else '', '; ' + c if c else '')
        return c

    def entries(hidden=False):
        """-> [([options], row)]: a member of a group joins the row of the first one unless it has cells of its own (class, why, evidence, pr, bench, advice)"""
        out, first = [], {}
        for k, r in notes.items():
            if r['applies'] == '-' and not hidden:
                continue
            g = r['group']
            if g in first and all(r[c] in ('', first[g][1][c]) for c in CELLS):
                first[g][0].append(by_key[k])
                continue
            out.append(([by_key[k]], r))
            if g:
                first.setdefault(g, out[-1])
        return out

    def prefix(os_):
        pre = os.path.commonprefix([m['name'] for m in os_])
        return pre[:max(pre.rfind('_'), pre.rfind('.')) + 1]

    def names(os_):
        return '`%s`' % os_[0]['name'] if len(os_) == 1 else '`%s{%s}`' % (prefix(os_), ','.join(m['name'][len(prefix(os_)):] for m in os_))

    def defaults(os_, marks=False):
        return default(os_[0], marks) if len(os_) == 1 else ', '.join('%s=%s' % (m['name'][len(prefix(os_)):], default(m, marks)) for m in os_)

    def ev(r, c):       # a member of a family that repeats the cell of the first one says so
        v = meta(r, c)
        g = lead[r['group'] or r['key']]
        return v if r is g or r[c] not in ('', g[c]) else ('as `%s`' % g['key'] if v else v)

    def heading(os_):
        return names(os_).replace('`', '') if len(os_) > 1 else os_[0]['key']

    def anchor(os_):
        return re.sub(r'\s', '-', re.sub(r'[^\w\s-]', '', heading(os_).lower()))

    n_opts = sum(1 for _ in entries())
    readme = ['<!-- sift-options:start -->', '## Options', '',
              '%d index options (`WITH (...)`), `sift.*` settings and automatic behaviors, each with its default, class (`default`, `opt-in`, `diagnostic`, `safety`, `deprecated`), measured gain and cost, and off switch, are in [docs/OPTIONS.md](docs/OPTIONS.md); types, ranges and contexts are in the tables of [docs/USAGE.md](docs/USAGE.md#maintenance-and-operations).' % n_opts,
              '<!-- sift-options:end -->']

    def formula_line(r):
        f = formula(lambda c: ev(r, c))
        if not f:
            return ''
        S, V, G, W, R, B, C, failed = f
        return 'S %.2fx saving %.0fk instructions, G %.1f%%, W %.1f%%, R %g: B %.1f against C %.1f, %s' % (S, V, G, W, R, B, C, 'automatic' if not failed else 'opt-in (fails: %s)' % ', '.join(failed))

    def shown(r, c):
        return '**not measured**' if ev(r, c) == 'not measured' else ev(r, c)

    sections = ['<!-- sift-cases:start -->', '| Class | Entries | Meaning |', '|---|---|---|']
    ents = entries(True)
    meaning = {'default': 'on by default: automatic behaviour, or a knob whose default is the recommended value', 'opt-in': 'off or empty by default; `why` names the term of the default formula it failed (or why the formula does not cover it)',
               'diagnostic': 'to measure, compare or explain a plan; not for a deployed configuration', 'safety': 'a limit or guard on memory, locks, I/O or replicas',
               'deprecated': 'an older spelling that is still accepted'}
    for c in CLASSES:
        sections.append('| [%s](#%s) | %d | %s |' % (c, c, sum(len(os_) for os_, r in ents if meta(r, 'class') == c), meaning[c]))
    for c in CLASSES:
        sub = [(os_, r) for os_, r in ents if meta(r, 'class') == c]
        if not sub:
            continue
        sections += ['', '### %s' % c]
        for os_, r in sub:
            why = ', '.join(WHY[w.strip()] for w in meta(r, 'why').split(',')) if meta(r, 'why') else ''
            bench = ev(r, 'bench')
            bench = bench if bench.startswith(('as `', 'n/a')) else '' if bench in ('', '-') else '`%s`' % bench if '/' not in bench else '[%s](%s)' % (bench, bench[5:])
            prs = ', '.join('[#%s](https://github.com/brauliobo/pg_sift/pull/%s)' % (n, n) for n in re.findall(r'\d+', r['pr'] or lead.get(r['group'] or '', r)['pr']))
            imp = ['index size: %s' % shown(r, 'growth'), 'build, merge and writes: %s' % shown(r, 'write')] + (['other: %s' % cost(r, False)] if meta(r, 'cost') not in ('', '-') else [])
            lines = ['', '#### %s' % heading(os_), '', '%s · %s%s · default %s' % ('`%s`' % r['applies'].replace(', ', '`, `') if r['applies'] != '-' else 'all', c, ' · since %s' % r['since'] if r['since'] else '', defaults(os_)), '',
                     '- **What it does:** %s' % meta(r, 'buys'),
                     '- **Gain:** %s' % shown(r, 'gain'),
                     *(['- **Formula:** ' + formula_line(r)] if formula_line(r) else []),
                     '- **Impact:** %s' % '; '.join(imp),
                     '- **When:** %s' % meta(r, 'when'),
                     '- **Default:** %s%s' % (defaults(os_), ' — ' + meta(r, 'default_why') if meta(r, 'default_why') else '')]
            if why:
                lines.append('- **Why not the default:** %s' % why)
            lines += ['- **Turn off:** %s' % meta(r, 'off'),
                      '- **Upgrade from %s:** %s' % (released_label(), r['upgrade'] or 'unchanged%s' % ('; writes column format minor %s' % r['minor'] if r['minor'] and int(r['minor']) > released_max_read() else '')),
                      '- **Advice:** %s' % meta(r, 'advice')]
            src = [x for x in (('PR ' + prs) if prs else '', ('benchmark ' + bench) if bench else '', link(r, '').replace('](', '](') if r['link'] else '') if x]
            if src:
                lines.append('- **Sources:** %s' % '; '.join(src))
            sections += lines
    sections.append('<!-- sift-cases:end -->')

    nt = ['<!-- sift-notices:start -->', '| Level | Message (`%s` is a placeholder) | Raised in | Options it names |', '|---|---|---|---|']
    for level, msg, path, named in notices(opts):
        nt.append('| %s | %s | `src/%s` | %s |' % (level, esc(re.sub(r'%[-+ 0#]*[0-9*]*(?:\.[0-9*]+)?(?:l|ll|z)?[a-zA-Z]', '%s', msg)), path, ', '.join('`%s`' % n for n in named) or ''))
    nt.append('<!-- sift-notices:end -->')

    up_new = [o['key'] for o in opts if not o['testing'] and o['key'] not in released]
    up_def = [o['key'] for o in opts if not o['testing'] and o['key'] in released and released[o['key']] != fmt(o, o['default'])]
    upgrade = ['<!-- sift-upgrade:start -->', '%s since %s; new: %s.' % ('No default changed' if not up_def else 'Defaults changed: ' + ', '.join(up_def), released_label(), ', '.join('`%s`' % k for k in up_new) or 'none')]
    if any(r['upgrade'] for r in notes.values()):
        upgrade += ['', '| Option | %s | %s | Column format | What a %s user gets after the update |' % (released_label(), release_label(), released_label()), '|---|---|---|---|---|']
    for k, r in notes.items():
        if r['upgrade']:
            o = by_key[k]
            fmt_ = ('minor %s%s' % (r['minor'], ', unreadable by %s' % released_label() if int(r['minor']) > released_max_read() else '')) if r['minor'] else ''
            upgrade.append('| ' + ' | '.join(esc(c) for c in ('`%s`' % k, 'automatic' if o['kind'] == 'auto' else released[k] if k in released else 'new', default(o, False), fmt_, r['upgrade'])) + ' |')
    upgrade.append('<!-- sift-upgrade:end -->')

    gs = ['| Setting | Applies to | Default | Class | Values | Context | Description | Cost |', '|---|---|---|---|---|---|---|---|']
    rs = ['| Index option | Index | Default | Class | Values | Description | Cost |', '|---|---|---|---|---|---|---|']
    for o, r in listed:
        c = (cost(r) + (' ' + link(r, '') if r['link'] else '')).strip()
        d = (o['short'] + ' ' + o['long']).strip()
        if o['kind'] == 'guc':
            gs.append('| `%s` | %s | %s | %s | %s | %s | %s | %s |' % (o['name'], r['applies'], default(o), meta(r, 'class'), values(o, r), o['ctx'], esc(d), esc(c)))
        else:
            rs.append('| `%s` | `%s` | %s | %s | %s | %s | %s |' % (o['name'], o['am'], default(o), meta(r, 'class'), values(o, r), esc(d), esc(c)))
    return ('\n'.join(readme), '\n'.join(['<!-- sift-settings:start -->'] + gs + ['<!-- sift-settings:end -->']), '\n'.join(['<!-- sift-reloptions:start -->'] + rs + ['<!-- sift-reloptions:end -->']),
            '\n'.join(sections), '\n'.join(upgrade), '\n'.join(nt))
