# Schema and strict source-sync checks for docs/options.tsv (compact 6.0 schema).
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
    if row['values']:
        return row['values']
    if o['type'] == 'bool':
        return '`true`, `false`' if o['kind'] == 'rel' else '`on`, `off`'
    if o['type'] == 'enum':
        return ', '.join('`%s`' % m for m in o['members'])
    if o['type'] == 'string':
        return 'text'
    if o['lo'] is None:
        return o['type']
    return '`%s..%s`%s' % (num(o['lo']), 'max' if o['hi'] == 2147483647 else num(o['hi']), ' ' + o['unit'] if o['unit'] else '')


def read_tsv(text, cols, name, errors):
    rows = {}
    for n, line in enumerate(text.split('\n'), 1):
        if not line.strip() or line.startswith('#'):
            continue
        f = line.split('\t')
        f += [''] * (len(cols) - len(f))
        if len(f) != len(cols):
            errors.append('%s:%d: %d columns, not %d (%s)' % (name, n, len(f), len(cols), ', '.join(cols)))
            continue
        r = dict(zip(cols, [x.strip() for x in f]), line=n)
        if r[cols[0]] in rows:
            errors.append('%s:%d: %s is twice' % (name, n, r[cols[0]]))
        rows[r[cols[0]]] = r
    return rows


NOTE_COLS = ['key', 'applies', 'values', 'purpose', 'link', 'since', 'replacement']
F1_ONLY = re.compile(r'Format 1 only \(deprecated in 6\.0; (?:replaced by (\w+)|(automatic)) in Format 2\.0\)')


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
    keys = {o['key'] for o in docs}
    for o in docs:
        if o['key'] not in notes:
            errors.append('%s: %s has no row in docs/options.tsv' % (o['where'], o['key']))
    for k, r in notes.items():
        where = 'docs/options.tsv:%d: %s' % (r['line'], k)
        if k not in keys:
            errors.append('%s is not an option of src/ (or it is under #ifdef SIFT_TESTING)' % where)
            continue
        for a in r['applies'].split(','):
            if a.strip() not in APPLIES:
                errors.append('%s: applies %s is none of %s' % (where, a, ', '.join(APPLIES)))
        if not r['purpose']:
            errors.append('%s: purpose is empty' % where)
        if len(r['purpose']) > 140:
            errors.append('%s: purpose is %d characters (max 140)' % (where, len(r['purpose'])))
        if r['since'] and not re.fullmatch(r'\d+\.\d+', r['since']):
            errors.append('%s: since must be MAJOR.MINOR (e.g. 6.0)' % where)
        o = next(o for o in docs if o['key'] == k)
        if (o['status'] == 'deprecated') != bool(r['replacement']):
            errors.append('%s: replacement must be set for a deprecated option of src/ and for no other' % where)
        m = F1_ONLY.match(o['short'])
        if m and r['replacement'] != (m.group(1) or 'automatic in Format 2.0'):
            errors.append('%s: replacement is %s but src/ says %s' % (where, r['replacement'] or 'empty', m.group(1) or 'automatic in Format 2.0'))
        if r['link']:
            f, _, anchor = r['link'].partition('#')
            if not os.path.exists(os.path.join(ROOT, 'docs', f)) or (anchor and anchor not in anchors(f)):
                errors.append('%s: link %s is not a valid file/heading of docs/' % (where, r['link']))
