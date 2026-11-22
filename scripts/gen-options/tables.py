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


def generate(opts, notes, released, version, old_version):
    by_key = {o['key']: o for o in opts if not o['testing']}

    gs = ['| Setting | Default | Range | Context | Purpose |', '|---|---|---|---|---|']
    for k in sorted(k for k, o in by_key.items() if o['kind'] == 'guc'):
        o, r = by_key[k], notes[k]
        d = '`%s`' % fmt(o, o['default'])
        if o['unit'] == 'kB' and o['type'] == 'int' and o['default'] and o['default'] % 1024 == 0:
            d = '`%d` (%d MB)' % (o['default'], o['default'] // 1024)
        gs.append('| `%s` | %s | %s | %s | %s |' % (
            o['name'], d, values(o, r), o['ctx'], esc(r['purpose']) + doc_ref(r)))

    col_rows = ['### Column Index (`sift_col`) — %d Reloptions' % sum(o['kind'] == 'rel' and o['am'] == 'sift_col' for o in by_key.values()),
                '', '| Option | Default | Values | Purpose |', '|---|---|---|---|']
    for k in sorted(k for k, o in by_key.items() if o['kind'] == 'rel' and o['am'] == 'sift_col'):
        o, r = by_key[k], notes[k]
        col_rows.append('| `%s` | `%s` | %s | %s |' % (
            o['name'], fmt(o, o['default']), values(o, r), esc(r['purpose']) + doc_ref(r)))

    sift_rows = ['', '### Full-Text Search Index (`sift`) — %d Reloptions' % sum(o['kind'] == 'rel' and o['am'] == 'sift' for o in by_key.values()),
                 '', '| Option | Default | Values | Purpose |', '|---|---|---|---|']
    for k in sorted(k for k, o in by_key.items() if o['kind'] == 'rel' and o['am'] == 'sift'):
        o, r = by_key[k], notes[k]
        sift_rows.append('| `%s` | `%s` | %s | %s |' % (
            o['name'], fmt(o, o['default']), values(o, r), esc(r['purpose']) + doc_ref(r)))

    tbl_rows = ['| Option | Default | Values | Purpose |', '|---|---|---|---|']
    for k in sorted(k for k, o in by_key.items() if o['kind'] == 'tbl'):
        o, r = by_key[k], notes[k]
        tbl_rows.append('| `%s` | `%s` | %s | %s |' % (
            o['name'], fmt(o, o['default']), values(o, r), esc(r['purpose']) + doc_ref(r)))

    return (
        '\n'.join(['<!-- sift-settings:start -->'] + gs + ['<!-- sift-settings:end -->']),
        '\n'.join(['<!-- sift-reloptions:start -->'] + col_rows + sift_rows + ['<!-- sift-reloptions:end -->']),
        '\n'.join(['<!-- sift-table-options:start -->'] + tbl_rows + ['<!-- sift-table-options:end -->']),
    )
