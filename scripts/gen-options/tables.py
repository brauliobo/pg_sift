# Compact 6.0 table renderers for docs/OPTIONS.md (5-col settings, 4-col reloptions, 4-col table options).
def release_label():
    return re.search(r"default_version\s*=\s*'(\d+\.\d+)", read('pg_sift.control')).group(1)


def snapshot(opts):
    return '\n'.join('%s\t%s' % (o['key'], fmt(o, o['default'])) for o in sorted(opts, key=lambda o: o['key']) if not o['testing'])


def doc_ref(r):
    if not r['link']:
        return ''
    f = r['link'].partition('#')[0]
    return ' ([%s](%s))' % (f, r['link'])


def purpose_cell(o, r):
    return esc(r['purpose']) + doc_ref(r)


def option_rows(by_key, notes, **match):
    return [(by_key[k], notes[k]) for k in sorted(k for k, o in by_key.items() if all(o[a] == v for a, v in match.items()))]


def reloption_rows(rows):
    return ['| `%s` | `%s` | %s | %s |' % (o['name'], fmt(o, o['default']), values(o, r), purpose_cell(o, r)) for o, r in rows]


def replacement_cell(r):
    return r['replacement'] if ' ' in r['replacement'] else '`%s`' % r['replacement']


def generate(opts, notes, released, version, old_version):
    by_key = {o['key']: o for o in opts if not o['testing']}
    col = option_rows(by_key, notes, kind='rel', am='sift_col', status='active')
    bm25 = option_rows(by_key, notes, kind='rel', am='sift', status='active')
    tbl = option_rows(by_key, notes, kind='tbl', status='active')
    old = [(o, notes[o['key']]) for am in ('sift_col', 'sift', 'sift_table') for o, _ in option_rows(by_key, notes, am=am, status='deprecated')]

    gs = ['| Setting | Default | Range | Context | Purpose |', '|---|---|---|---|---|']
    for o, r in option_rows(by_key, notes, kind='guc'):
        d = '`%s`' % fmt(o, o['default'])
        if o['unit'] == 'kB' and o['type'] == 'int' and o['default'] and o['default'] % 1024 == 0:
            d = '`%d` (%d MB)' % (o['default'], o['default'] // 1024)
        gs.append('| `%s` | %s | %s | %s | %s |' % (o['name'], d, values(o, r), o['ctx'], purpose_cell(o, r)))

    head = ['| Option | Default | Values | Purpose |', '|---|---|---|---|']
    col_rows = ['### Column Index (`sift_col`) — %d Reloptions' % len(col), ''] + head + reloption_rows(col)
    sift_rows = ['', '### Full-Text Search Index (`sift`) — %d Reloptions' % len(bm25), ''] + head + reloption_rows(bm25)
    old_rows = ['| Option | Applies to | Replaced by in 6.0 | Former purpose |', '|---|---|---|---|']
    old_rows += ['| `%s` | `%s` | %s | %s |' % (o['name'], o['am'], replacement_cell(r), esc(r['purpose'])) for o, r in old]

    return (
        '\n'.join(['<!-- sift-settings:start -->'] + gs + ['<!-- sift-settings:end -->']),
        '\n'.join(['<!-- sift-reloptions:start -->'] + col_rows + sift_rows + ['<!-- sift-reloptions:end -->']),
        '\n'.join(['<!-- sift-table-options:start -->'] + head + reloption_rows(tbl) + ['<!-- sift-table-options:end -->']),
        '\n'.join(['<!-- sift-deprecated-options:start -->'] + old_rows + ['<!-- sift-deprecated-options:end -->']),
    )
