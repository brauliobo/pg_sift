# The rows of docs/options.tsv and the checks that every option has its row, its measured cells and its place in the default formula.
# Run into the namespace of main.py (exec), not imported; not a script of its own.
def num(v):
    return str(int(v)) if v == int(v) else '%g' % v


def heuristic(key):
    """the option-shaped record of a row `auto(name)`: an automatic behavior that has no setting of its own"""
    return {'key': key, 'name': key, 'kind': 'auto', 'type': 'auto', 'am': None, 'default': None, 'unit': '', 'ctx': '', 'short': '', 'long': '', 'where': 'docs/options.tsv', 'testing': False}


def fmt(o, v):
    if o['type'] == 'auto':
        return 'automatic'
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
    if o['type'] == 'auto':
        return ''
    if o['type'] == 'bool':
        return 'bool'
    if o['type'] == 'enum':
        return 'enum: ' + ', '.join(o['members'])
    if o['type'] == 'string':
        return row['values'] or 'text'
    return '%s %s..%s%s' % (o['type'], num(o['lo']), 'max' if o['hi'] == 2147483647 else num(o['hi']), ' ' + o['unit'] if o['unit'] else '')


# ---- the notes ----

UNITS = r'(?:%|x\b|ms\b|s\b|us\b|µs\b|MiB\b|MB\b|GB\b|M\b|instructions\b)'


def figures(text):
    """the measured figures of a cell: a number with a decimal point or with a unit (ms, MB, %, x, M instructions ..), as floats; the sizes of a data set (`200,000 rows`) and what is in backticks are not figures"""
    t = re.sub(r'`[^`]*`', '', text)
    t = re.sub(r'\bv?\d+\.\d+\.\d+\b', '', t)
    t = re.sub(r'(\d),(\d{3})', r'\1\2', t)
    t = re.sub(r'(\d),(\d{3})', r'\1\2', t)
    return {float(m.group(1)) for m in re.finditer(r'(?<![\w.])(\d+(?:\.\d+)?)(?:\s*%s)' % UNITS, t)} | {float(x) for x in re.findall(r'(?<![\w.])(\d+\.\d+)(?![\w])', t)}


def table_figures(text):
    """every number of a results table, and a size in bytes as KB, MB and MiB (a cell quotes 4.05 MB for 4046848 bytes)"""
    t = re.sub(r'(\d),(\d{3})', r'\1\2', text)
    out = set()
    for x in re.findall(r'(?<![\w.])-?\d+(?:\.\d+)?(?![\w])', t):
        v = abs(float(x))
        out |= {v, round(v, 1), float(round(v))}
        if v >= 10000:
            for d in (1e3, 1e6, 1024.0, 1048576.0):
                out |= {round(v / d, k) for k in (0, 1, 2)}
    return out

def read_tsv(text, cols, name, errors):
    rows = {}
    for n, line in enumerate(text.split('\n'), 1):
        if not line.strip() or line.startswith('#'):
            continue
        f = line.split('\t')
        f += [''] * (len(cols) - len(f))        # trailing empty cells may be stripped
        if len(f) != len(cols):
            errors.append('%s:%d: %d columns, not %d (%s)' % (name, n, len(f), len(cols), ', '.join(cols)))
            continue
        r = dict(zip(cols, [x.strip() for x in f]), line=n)
        if r[cols[0]] in rows:
            errors.append('%s:%d: %s is twice' % (name, n, r[cols[0]]))
        rows[r[cols[0]]] = r
    return rows


NOTE_COLS = ['key', 'applies', 'group', 'values', 'minor', 'buys', 'cost', 'link', 'since', 'class', 'why', 'gain', 'growth', 'write', 's', 'save', 'g', 'w', 'r', 'pr', 'bench', 'advice', 'upgrade', 'when', 'default_why', 'off']
CELLS = ('class', 'why', 'gain', 'growth', 'write', 's', 'save', 'g', 'w', 'r', 'pr', 'bench', 'advice', 'when', 'default_why', 'off')
DOCUMENTED = {'buys': 'what it does', 'gain': 'the measured gain', 'growth': 'the impact on the index size', 'write': 'the impact on build, merge and write cost', 'cost': 'the other impact: planning, memory, WAL',
              'when': 'when it applies and when it does not', 'default_why': 'the default and why', 'off': 'how to turn it off'}
CLASSES = ['default', 'opt-in', 'diagnostic', 'safety', 'deprecated']
# the terms of the rule for a default (docs/OPTIONS.md, "How a default is chosen") that an opt-in failed, and the reasons the rule does not cover
WHY = {'gain': 'the speedup is under 1.05, or saves under 100k instructions a query', 'cost': 'the gain does not pay for the growth and the build', 'ceiling': 'the index grows over 25%',
       'detect': 'no statistic says which columns it pays on', 'result': 'changes results or semantics', 'hardware': 'needs hardware', 'memory': 'takes memory',
       'risk': 'not run on real workloads', 'unknown': 'the history does not say'}
FORMULA = ('s', 'save', 'g', 'w', 'r')      # measured in instructions: speedup of the relevant class, instructions saved a query (thousands), growth %, write/build/merge cost %, relevance 1 or 0.5
SAVE_MIN, S_MIN = 100, 1.05


def number(v):
    try:
        return float(v)
    except ValueError:
        return None


def formula(cell):
    """-> (S, save, G, W, R, B, C, [terms failed]) from the cells s, save, g, w, r; None when one is not a number (`-` is 0 for g and w, 1 for s; `n/a: reason` when the formula does not decide the option) or not measured"""
    s, save, g, w, r = (cell(c) for c in FORMULA)
    S = 1.0 if s == '-' else number(s)
    G = 0.0 if g == '-' else number(g)
    W = 0.0 if w == '-' else number(w)
    R, V = number(r), number(save) if save != '-' else 0.0
    if None in (S, G, W, R, V):
        return None
    B = R * math.log2(S) / math.log2(1.05) if S > 1 else 0.0
    C = G / 3 + W / 5
    failed = (['gain'] if S < S_MIN or V < SAVE_MIN else []) + (['cost'] if B < C else [])
    return S, V, G, W, R, B, C, failed
EVIDENCE = ('gain', 'growth', 'write')     # empty: not measured (--missing); `-`: no such effect


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


def check(opts, notes, errors, released, srcs=None):
    docs = [o for o in opts if not o['testing']]
    auto = {k for k in notes if re.fullmatch(r'auto\(.+\)', k)}
    keys = {o['key'] for o in docs} | auto
    minor_now = next((o['default'] for o in docs if o['key'] == 'sift.col_format_minor'), 0)
    # an option may stamp a minor above the default (point_tid_columns: 16, key_cut: 17, order_columns: 18, hll_columns: 20), which the library reads (SIFT_COL_FORMAT_MAX_READ): the newest one defined
    colh = ((srcs or {}).get('col/col.h') or read('src/col/col.h')) + ((srcs or {}).get('col/api/format.h') or read('src/col/api/format.h'))
    minors = [int(x) for x in re.findall(r'#define SIFT_COL_FORMAT_[A-Z0-9_]+\s+(\d+)\b', colh)]
    minor_max = max([int(minor_now)] + minors)
    # every minor the library reads has the release that first reads it in scripts/upgrade_check.sql (col_first): without it upgrade_check('3.x') takes it for 3.0's and cannot block an index that 3.x cannot read
    first = json.loads(re.search(r"col_first CONSTANT jsonb := '(\{.*?\})'", read('scripts/upgrade_check.sql')).group(1))
    for n in range(int(re.search(r'#define SIFT_COL_FORMAT_MIN_READ\s+(\d+)', colh).group(1)), minor_max + 1):
        if str(n) not in first:
            errors.append('scripts/upgrade_check.sql: col_first has no entry for column format minor %d (the release that first reads it)' % n)
    for o in docs:
        if o['key'] not in notes:
            errors.append('%s: %s has no row in docs/options.tsv (applies, buys, cost, link: what it is for, what it costs; `-` in applies hides an alias)' % (o['where'], o['key']))
    group_first = {}
    cases = {l.split('\t')[0] for l in read('bench/cases.tsv').split('\n') if l.strip() and not l.startswith('#')}
    for k, r in notes.items():
        n = 'docs/options.tsv:%d: %s' % (r['line'], k)
        if k not in keys:
            errors.append('%s is not an option of src/ (or it is under #ifdef SIFT_TESTING)' % n)
            continue
        first = group_first.setdefault(r['group'], r) if r['group'] else r
        o = heuristic(k) if k in auto else next(o for o in docs if o['key'] == k)
        cls = r['class'] or first['class']
        if cls not in CLASSES:
            errors.append('%s: class `%s` is none of %s' % (n, cls, ', '.join(CLASSES)))
        if not (r['advice'] or first['advice']):
            errors.append('%s: no advice (the line a user reads: when to set it, when to leave it, how to opt out)' % n)
        if cls == 'opt-in' and not (r['why'] or first['why']):
            errors.append('%s: an opt-in needs why: the term of the default formula it failed or the reason the formula does not cover (%s)' % (n, ', '.join(WHY)))
        whys = [w.strip() for w in (r['why'] or first['why']).split(',')] if (r['why'] or first['why']) else []
        for w in whys:
            if w not in WHY:
                errors.append('%s: why %s is none of %s' % (n, w, ', '.join(WHY)))
        for c in FORMULA:
            v = r[c] or first[c]
            if v not in ('', '-') and number(v) is None and not v.startswith('n/a'):
                errors.append('%s: %s is a number (`-` for none, `n/a: reason`, empty for not measured), not `%s`' % (n, c, v))
        f = formula(lambda c: r[c] or first[c])
        if f and cls in ('default', 'opt-in'):
            S, V, G, W, R, B, C, failed = f
            line = 'S %.2f saving %.0fk instructions, G %.1f%%, W %.1f%%, R %g: B %.1f against C %.1f' % (S, V, G, W, R, B, C)
            if cls == 'opt-in' and not failed and 'detect' not in whys:
                errors.append('%s: an opt-in that passes the default formula (%s): make it a default with its detection' % (n, line))
            if cls == 'opt-in' and not set(failed) <= set(whys):
                errors.append('%s: why must name %s, the terms of the formula it fails (%s)' % (n, ', '.join(sorted(set(failed) - set(whys))), line))
            if cls == 'default' and failed and not (r['default_why'] or first['default_why']).startswith('Formula waived:'):
                errors.append('%s: a default that fails the formula on %s (%s); opt-in, or `Formula waived: reason` in default_why' % (n, ', '.join(failed), line))
        if r['pr'] and not re.fullmatch(r'\d+(, ?\d+)*', r['pr']):
            errors.append('%s: pr is a comma separated list of pull request numbers' % n)
        if r['since'] and not re.fullmatch(r'\d+\.\d+', r['since']):
            errors.append('%s: since is a release like 3.3' % n)
        b = r['bench']
        for c in EVIDENCE + FORMULA + ('bench',):
            if r[c].startswith('n/a') and not re.fullmatch(r'n/a: \S.*', r[c]):
                errors.append('%s: %s says n/a without the reason (`n/a: why there is nothing to measure`)' % (n, c))
        bf, _, ba = b.partition('#')
        t = b[5:] if b.startswith('test:') else ''
        if b.startswith('test:'):
            if not (os.path.exists(os.path.join(ROOT, 'test/sql', t + '.sql')) or glob.glob(os.path.join(ROOT, 'test/t', t + '.*'))):
                errors.append('%s: bench %s is no regress test (test/sql) or TAP test (test/t)' % (n, b))
        elif b and b != '-' and not b.startswith('n/a') and b not in cases and not (bf.startswith('docs/') and os.path.exists(os.path.join(ROOT, bf)) and (not ba or ba in anchors(bf[5:]))):
            errors.append('%s: bench %s is neither a case of bench/cases.tsv, `test:NAME`, `n/a: reason`, `-`, nor a file%s of docs/ (written docs/results/x.tsv)' % (n, b, ' and heading' if ba else ''))
        if bf.startswith('docs/results/') and os.path.exists(os.path.join(ROOT, bf)):
            have = table_figures(read(bf))
            for c in ('gain', 'growth', 'write'):
                lost = sorted(f for f in figures(r[c] or first[c]) if f not in have)
                if lost:
                    errors.append('%s: %s has figures that no row of %s holds (%s): quote a measured value of the table, or remove the figure' % (n, c, bf, ', '.join('%g' % f for f in lost[:6])))
        if o['type'] == 'bool' and cls in ('default', 'opt-in') and (cls == 'opt-in') == bool(o['default']):
            errors.append('%s: class %s but the default is %s' % (n, cls, 'on' if o['default'] else 'off'))
        changed = (tuple(map(int, r['since'].split('.'))) > tuple(map(int, released_label().split('.')))) if k in auto else (o['key'] not in released or released[o['key']] != fmt(o, o['default']) or (r['minor'] and int(r['minor']) > released_max_read()))
        if changed and not r['upgrade']:
            errors.append('%s: %s since the last release: say in `upgrade` what a user of that release gets after ALTER EXTENSION UPDATE and whether it needs a REINDEX' % (n, 'is new' if k in auto or o['key'] not in released else 'has another default' if released[o['key']] != fmt(o, o['default']) else 'writes a newer column format minor'))
        for c in DOCUMENTED:        # the seven things the documentation says of every option and heuristic (what it does, gain, impact, when, default and why, off switch, upgrade)
            v = r[c] or first[c]
            if not v or re.match(r'(?i)tbd\b', v):
                errors.append('%s: no %s (%s; `-` or `n/a: reason` when there is nothing to say, never empty or tbd)' % (n, c, DOCUMENTED[c]))
        for ref in re.findall(r'\b(sift\.[a-z_0-9]+)\s*(?:=|TO\b)', r['off'] or first['off']):
            if ref not in keys:
                errors.append('%s: off names %s, which is no setting' % (n, ref))
        if r['applies'] == '-':
            if cls != 'deprecated':
                errors.append('%s: applies `-` is an alias that is not listed: class deprecated' % n)
            if not r['buys']:
                errors.append('%s: applies `-` needs the reason in buys' % n)
            continue
        for a in r['applies'].split(','):
            if a.strip() not in APPLIES:
                errors.append('%s: applies %s is none of %s' % (n, a, ', '.join(APPLIES)))
        for c in ('buys', 'cost'):
            if not (r[c] or first[c]):
                errors.append('%s: no %s' % (n, c))
        if r['minor'] and not ((o['am'] == 'sift_col' or k in auto) and r['minor'].isdigit() and 7 <= int(r['minor']) <= minor_max):
            errors.append('%s: minor %s is not a column format minor of a sift_col option (7 to %s)' % (n, r['minor'], minor_max))
        if o['type'] == 'string' and not r['values']:
            errors.append('%s: a string option needs its values (the grammar in a few words)' % n)
        if r['link']:
            f, _, anchor = r['link'].partition('#')
            if not os.path.exists(os.path.join(ROOT, 'docs', f)) or (anchor and anchor not in anchors(f)):
                errors.append('%s: the link %s is not a file%s of docs/' % (n, r['link'], ' and heading' if anchor else ''))
