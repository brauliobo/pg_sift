#!/usr/bin/env python3
"""Stored results of the 3.1 and 3.2 benchmarks (docs/results/*.tsv, one file per benchmark) and the generated blocks of docs/BENCHMARKS.md and README.md.

    extras.py render [--results DIR] [--benchmarks FILE] [--readme FILE]      rewrite every block that has a results file (bench/bench extras report)

A results file is `# title`, `# key: value` lines (the run: build, host, load, method) and a tab separated table:

    version  case  arm  metric  value  min  max  n  load  note

`case` is the point of the benchmark (a size, a query), `arm` what was measured there (a plan, a build, an engine), `metric` its unit's name
(`ms`, `bytes`, `rows_per_s`, ...), `load` the 1-minute load average while it ran. The runners (steps/sets.py, steps/extras.sh) write the files through
`write`; `render` only reads them, so no number of a block is typed by hand. Block markers are `<!-- bench:NAME:begin ... -->` / `<!-- bench:NAME:end -->`
(readme.replace_block): sets, rollup, writepath, bm25size, leaftypes, fields, buildcost, gpu.
"""
import argparse
import csv
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from readme import replace_block as _replace_block  # noqa: E402

REPO = os.path.dirname(os.path.dirname(HERE))
COLUMNS = ['version', 'case', 'arm', 'metric', 'value', 'min', 'max', 'n', 'load', 'note']


def host_load():
    pressure = ''
    try:
        pressure = ' cpu_psi %s%%' % re.search(r'some avg10=([0-9.]+)', open('/proc/pressure/cpu').read()).group(1)
    except (OSError, AttributeError):
        pass
    return 'load %s%s' % (' '.join(open('/proc/loadavg').read().split()[:3]), pressure)


def write(path, title, meta, rows):
    os.makedirs(os.path.dirname(os.path.abspath(path)), exist_ok=True)
    with open(path, 'w') as f:
        f.write('# %s\n' % title)
        for k, v in meta.items():
            f.write('# %s: %s\n' % (k, v))
        f.write('\t'.join(COLUMNS) + '\n')
        for r in rows:
            f.write('\t'.join(str(r.get(c, '')) for c in COLUMNS) + '\n')


def read(path):
    """(meta, rows) of a results file"""
    meta, lines = {}, []
    for line in open(path):
        if line.startswith('#'):
            m = re.match(r'# ([\w ]+): (.*)', line.rstrip('\n'))
            if m:
                meta[m.group(1)] = m.group(2)
            else:
                meta.setdefault('title', line[2:].strip())
        elif line.strip():
            lines.append(line.rstrip('\n'))
    return meta, list(csv.DictReader(lines, delimiter='\t'))


def replace_block(path, name, lines):
    _replace_block(path, name, lines)


def get(rows, **kw):
    for r in rows:
        if all(r[k] == v for k, v in kw.items()):
            return r
    return None


def num(rows, **kw):
    r = get(rows, **kw)
    return None if r is None or r['value'] in ('', 'NA') else float(r['value'])


def ms(v):
    return 'n/a' if v is None else '%.2f' % v if v < 10 else '%.1f' % v if v < 100 else '%d' % round(v)


def table(head, body, align=None):
    align = align or ['---'] * len(head)
    out = ['| ' + ' | '.join(head) + ' |', '|' + '|'.join(align) + '|']
    return out + ['| ' + ' | '.join(r) + ' |' for r in body]


def footer(meta):
    return ('Measured %s on %s, %s; host %s at the start and %s at the end.' % (
        meta.get('date', '?'), meta.get('cpu', '?'), meta.get('postgresql', '?'), meta.get('host_load_before', '?'), meta.get('host_load_after', '?')))


# ---- sets -------------------------------------------------------------------------------------------------------------------------------

SET_ARMS = [
    ('array_generic', '`entity_id = ANY($1::int4[])`, generic plan (above 2k ids: index and bitmap scans off)'),
    ('array_custom', '`= ANY($1::int4[])`, custom plan (the array is a constant), default costs'),
    ('literal_in', 'literal `IN (1, 2, ...)`, default costs'),
    ('literal_in_agg', 'literal `IN` list, `SiftAgg` kept (page costs raised)'),
    ('normal_plan', 'literal `IN` list, normal plan (`sift.enable_agg = off`)'),
    ('member_in', '`IN (SELECT entity_id FROM memberships WHERE group_id = ...)`, default costs'),
    ('member_in_agg', 'the same, `SiftAgg` kept'),
    ('member_in_agg_lut_off', 'the same, `sift.agg_semi_lut = off`'),
    ('ch_literal_ins', 'ClickHouse, literal list, table in the order of id'),
    ('ch_literal_tuned', 'ClickHouse, literal list, partitioned by month, ordered by (source, entity, date, id)'),
    ('ch_member_tuned', 'ClickHouse, `IN (SELECT ...)` of the membership table, same table'),
]
SET_SIZES = ['1k', '2k', '5k', '10k', '100k']


def cell(rows, arm, size):
    r = get(rows, arm=arm, case=size)
    if r is None:
        return 'n/a'
    note = r['note']
    return ms(float(r['value'])) + (' (%s)' % note.replace(' RESULT DIFFERS', '!') if note and not note.startswith('ClickHouse') else '')


def sets_section(meta, rows):
    body = [['`%s`' % a, d] + [cell(rows, a, s) for s in SET_SIZES] for a, d in SET_ARMS if get(rows, arm=a)]
    sift = rows[0]['version'] if rows else '?'
    lines = [
        '3,000,000 facts of 200,000 entities (facts per entity: zipf, p50 5, p90 20, p99 135, the largest about 85,000), 30 sources, 40 languages, 1.5%% soft-deleted; the set is the members '
        'of one group of a 334,000-row membership table (60 groups of 100, 20 of 1,000, 4 of 2,000, 4 of 5,000, 8 of 10,000, 2 of 100,000). The sets of 1k to 5k ids are the ones the workload '
        'has today; 10k and 100k are headroom. One client, the median of %s timed runs of the statement '
        '`SELECT source_id, count(*), avg(score) ... WHERE deleted_at IS NULL AND fact_date >= <12 months ago> AND entity_id <set> GROUP BY 1`, in milliseconds, planning included '
        '(lower is better); the plan the statement ran is in brackets. Build: pg_sift %s. %s %s' % (rows[0]['n'] if rows else '?', meta.get('build', sift), footer(meta), meta.get('method', '')),
        '',
    ] + table(['arm', 'statement'] + ['%s ids' % s for s in SET_SIZES], body, ['---', '---'] + ['---:'] * len(SET_SIZES))
    types = ['int4', 'int8', 'text', 'date']
    trows = []
    for t in types:
        sfx = '' if t == 'int4' else '_' + t
        row = ['`%s`' % t]
        for arm in ('array_generic', 'normal_plan'):
            for s in ('1k', '5k'):
                row.append(cell(rows, arm + sfx, s))
        trows.append(row)
    unsupported = [(r['case'], r['note']) for r in rows if r['arm'] == 'sift_col_key' and r['value'] == '0']
    lines += ['', 'The same statement with the key column of another type, the array as a parameter of a generic plan (`= ANY($1::T[])`, index and bitmap scans off at 5k) against the literal list on the normal plan '
              '(same rows, key mapped from the entity id):', '']
    lines += table(['key type', 'array 1k', 'array 5k', 'normal plan 1k', 'normal plan 5k'], trows, ['---', '---:', '---:', '---:', '---:'])
    for t, note in unsupported:
        lines += ['', '`%s` is not supported by `sift_col` on this build (`%s`).' % (t, re.sub(r'^ERROR:\s*|\s*HINT.*$', '', note).strip())]
    lines += ['', 'Reproduce: `bench/bench sets` (data: `bench/sql/sets/gen.sql`, deterministic; results: [results/sets.tsv](results/sets.tsv)).']
    return lines


# ---- shared pieces of the sections ------------------------------------------------------------------------------------------------------

def versions(rows):
    """the builds of a results file in the order of their first row"""
    seen = []
    for r in rows:
        if r['version'] not in seen:
            seen.append(r['version'])
    return seen


def mib(b):
    return 'n/a' if b is None else '%.1f' % (b / 1048576)


def pct(new, old):
    return 'n/a' if not old or new is None else '%+.1f%%' % ((new / old - 1) * 100)


def times(new, old):
    return 'n/a' if not new or old is None else '%.1f×' % (old / new) if old / new >= 10 else '%.2f×' % (old / new)


def intro(meta, rows, what):
    return ['%s %s %s' % (what, footer(meta), meta.get('method', '')), '']


# ---- rollup -----------------------------------------------------------------------------------------------------------------------------

def rollup_section(meta, rows):
    sizes = [k for k in ('100k', '1M', '10M') if get(rows, case='j02@' + k)]
    q, idx = [], []
    for k in sizes:
        for name in ('j02', 'j17'):
            v = [num(rows, case='%s@%s' % (name, k), arm=a, note='rollup=default') for a in ('normal', 'scanned', 'rollup')]
            q.append([k, '`%s`' % name, ms(v[0]), ms(v[1]), ms(v[2]), times(v[1], v[0]), times(v[2], v[1])])
        d, o = (num(rows, case='index@' + k, arm=a, metric='bytes') for a in ('default', 'off'))
        c = [num(rows, case='build@' + k, arm=a, metric='cpu_ms') for a in ('default', 'off')]
        w = [num(rows, case='build@' + k, arm=a, metric='wall_ms_2_workers') for a in ('default', 'off')]
        idx.append([k, mib(o), mib(d), pct(d, o), ms(c[1]), ms(c[0]), pct(c[0], c[1]), ms(w[1]), ms(w[0])])
    lines = intro(meta, rows, 'Documents of 3 to 6 string elements per array (bench/sql/jsonb/gen.sql). `normal` is the plan of PostgreSQL without sift, `scanned` the `sift_col` scan pushdown that '
                  'reads every unit (`sift.agg_preagg` off), `rollup` the answer from the rollups stored with the index; the three are separate arms of one statement, in milliseconds, lower is better.')
    lines += table(['documents', 'query', 'normal', 'scanned', 'rollup', 'scanned vs normal', 'rollup vs scanned'], q, ['---', '---', '---:', '---:', '---:', '---:', '---:'])
    lines += ['', 'The cost of the rollups: the index with `rollup = \'off\'` against the default, `CREATE INDEX` on one backend (CPU ms of the backend) and with 2 workers (wall ms).', '']
    lines += table(['documents', 'index MiB, off', 'index MiB, default', 'index size', 'build CPU ms, off', 'build CPU ms, default', 'build CPU', 'wall ms, off, 2 workers', 'wall ms, default, 2 workers'],
                   idx, ['---', '---:', '---:', '---:', '---:', '---:', '---:', '---:', '---:'])
    return lines + ['', 'Reproduce: `bench/bench extras rollup` (results: [results/rollup.tsv](results/rollup.tsv)).']


# ---- sift_table writes ------------------------------------------------------------------------------------------------------------------

def tablewrites_section(meta, rows):
    old, new = (versions(rows) + [None, None])[:2]
    ops = []
    for r in rows:
        if r['arm'] == 'heap' and r['metric'] == 'ms' and r['case'] not in ops:
            ops.append(r['case'])
    body = []
    for c in ops:
        h = num(rows, version=new, case=c, arm='heap', metric='ms')
        t0, t1 = (num(rows, version=v, case=c, arm='sift_table', metric='ms') for v in (old, new))
        body.append([c, ms(h), ms(t0), ms(t1), pct(t1, t0), '%.2f' % (t1 / h) if h and t1 is not None else 'n/a'])
    lines = intro(meta, rows, 'The same orders loaded into a heap table and into a `sift_table` (`bench/tbl.sh`), one client, milliseconds, lower is better; the heap column is the %s run.' % new)
    lines += table(['operation', 'heap', 'sift_table %s' % old, 'sift_table %s' % new, '%s vs %s' % (new, old), 'sift_table / heap'], body, ['---', '---:', '---:', '---:', '---:', '---:'])
    rate = [num(rows, version=v, case='load rate', arm=a, metric='rows_per_s') for v, a in ((new, 'heap'), (old, 'sift_table'), (new, 'sift_table'))]
    size = {(v, a): (num(rows, version=v, case='size', arm=a, metric='table_mb'), num(rows, version=v, case='size', arm=a, metric='indexes_mb')) for v in (old, new) for a in ('heap', 'sift_table')}
    lines += ['', 'Load rate (rows per second, higher is better): heap %s, sift_table %s on %s, %s on %s. Size in MB, table / indexes: heap %s / %s; sift_table %s / %s on %s, %s / %s on %s.' % (
        '%d' % rate[0], '%d' % rate[1], old, '%d' % rate[2], new, '%d' % size[(new, 'heap')][0], '%d' % size[(new, 'heap')][1],
        '%d' % size[(old, 'sift_table')][0], '%d' % size[(old, 'sift_table')][1], old, '%d' % size[(new, 'sift_table')][0], '%d' % size[(new, 'sift_table')][1], new)]
    big = [r for r in rows if r['case'].startswith(('COPY', 'VACUUM after', 'stress'))]
    if big:
        lines += ['', 'One transaction and a long run (`bench/tbl-bigcopy.sh`, `bench/tbl-stress.sh`):', '']
        names = []
        for r in big:
            if (r['case'], r['metric']) not in names:
                names.append((r['case'], r['metric']))
        value = lambda v, c, m: 'n/a' if num(rows, version=v, case=c, metric=m) is None else '%g' % num(rows, version=v, case=c, metric=m)  # noqa: E731
        lines += table(['case', 'measure', old, new], [[c, m, value(old, c, m), value(new, c, m)] for c, m in names], ['---', '---', '---:', '---:'])
    return lines + ['', 'Reproduce: `bench/bench extras tablewrites` (results: [results/tablewrites.tsv](results/tablewrites.tsv)).']


def writepath_section(meta, rows):
    keys = []
    for r in rows:
        if r['metric'] not in keys:
            keys.append(r['metric'])
    body, seen = [], []
    for r in rows:
        k = (r['case'], r['arm'])
        if k in seen:
            continue
        seen.append(k)
        body.append([r['case'], '-' if r['arm'] == '-' else r['arm'], ', '.join('%s %s' % (m.replace('_', ' '), get(rows, case=k[0], arm=k[1], metric=m)['value']) for m in keys if get(rows, case=k[0], arm=k[1], metric=m))])
    return intro(meta, rows, 'Single-row `INSERT` statements into a `sift_table` with a primary key, in one transaction (`bench/tbl-writepath.sh`, build %s).' % meta.get('build', '?')) + table(['mode', 'access method', 'measurements'], body) + [
        '', 'Reproduce: `bench/bench extras writepath` (results: [results/writepath.tsv](results/writepath.tsv)).']


# ---- BM25 index size --------------------------------------------------------------------------------------------------------------------

def bm25size_section(meta, rows):
    old, new = (versions(rows) + [None, None])[:2]
    size, cost = [], []
    for c in ('titles', 'articles'):
        for arm, label in (('default', 'default'), ('positions_off', '`positions = off`')):
            b = [num(rows, version=v, case=c, arm=arm, metric='index_bytes') for v in (old, new)]
            cpu = [num(rows, version=v, case=c, arm=arm, metric='build_cpu_ms') for v in (old, new)]
            q = [num(rows, version=v, case=c, arm=arm, metric=m) for m in ('count_ms', 'top10_ms') for v in (old, new)]
            size.append([c, label, mib(b[0]), mib(b[1]), pct(b[1], b[0])])
            cost.append([c, label, ms(cpu[0] / 1000 if cpu[0] else None), ms(cpu[1] / 1000 if cpu[1] else None), pct(cpu[1], cpu[0])] + [ms(x) for x in q])
    lines = intro(meta, rows, 'The size of the BM25 index on two corpora, %s against %s.' % (old, new))
    lines += table(['corpus', 'index', 'MiB %s' % old, 'MiB %s' % new, 'change'], size, ['---', '---', '---:', '---:', '---:'])
    lines += ['', 'Build (CPU seconds of one backend) and two queries (median ms, `a AND b` count and top 10 by BM25):', '']
    lines += table(['corpus', 'index', 'build s %s' % old, 'build s %s' % new, 'change', 'count %s' % old, 'count %s' % new, 'top 10 %s' % old, 'top 10 %s' % new], cost,
                   ['---', '---', '---:', '---:', '---:', '---:', '---:', '---:', '---:'])
    return lines + ['', 'Reproduce: `bench/bench extras bm25size` (`scripts/bm25-size.sh` is the same measurement on the standard corpora; results: [results/bm25size.tsv](results/bm25size.tsv)).']


# ---- leaf types -------------------------------------------------------------------------------------------------------------------------

def leaftypes_section(meta, rows):
    vs = versions(rows)
    idx = []
    for v in vs:
        for kind in ('narrow', 'wide'):
            b = num(rows, version=v, case='index:' + kind, metric='bytes')
            if b is not None:
                idx.append([v, kind, mib(b), ms(num(rows, version=v, case='index:' + kind, metric='cpu_ms')), ms(num(rows, version=v, case='index:' + kind, metric='wall_ms'))])
    body = []
    for v in vs:
        for r in rows:
            if r['version'] == v and r['arm'] == 'gt normal':
                t = r['case']
                g = [num(rows, version=v, case=t, arm=a) for a in ('gt normal', 'gt siftagg', 'btw normal', 'btw siftagg')]
                body.append([v, '`%s`' % t.split(':', 1)[1], t.split(':')[0], ms(g[0]), ms(g[1]), times(g[1], g[0]), ms(g[2]), ms(g[3]), times(g[3], g[2]), get(rows, version=v, case=t, arm='gt siftagg')['note']])
    lines = intro(meta, rows, 'Columns of every type hold the same values; one `sift_col` index per side (narrow: `numeric(18,2)`, `int8`; wide: `numeric(30,6)`, `numeric`, `money`, `interval`).')
    lines += table(['build', 'index', 'MiB', 'CREATE INDEX CPU ms', 'wall ms'], idx, ['---', '---', '---:', '---:', '---:'])
    lines += ['', 'A filter that keeps the top 10% (`>`) and one that keeps 1% (`BETWEEN`), `count(*)` and a sum: the normal plan against `Custom Scan (SiftAgg)` (ms, lower is better; the plan in the last column is the one of the `>` filter):', '']
    lines += table(['build', 'type', 'path', '`>` normal', '`>` SiftAgg', 'speed-up', '`BETWEEN` normal', '`BETWEEN` SiftAgg', 'speed-up', 'plan'], body,
                   ['---', '---', '---', '---:', '---:', '---:', '---:', '---:', '---:', '---'])
    return lines + ['', 'Reproduce: `bench/bench extras leaftypes` (results: [results/leaftypes.tsv](results/leaftypes.tsv)).']


# ---- facets and fields ------------------------------------------------------------------------------------------------------------------

def fields_section(meta, rows):
    fac, arms = [], ['facet_counts', 'four GROUP BYs, SiftAgg', 'four GROUP BYs, normal plan']
    for r in rows:
        if r['case'].startswith('facets: ') and r['arm'] == arms[0]:
            c = r['case']
            v = [num(rows, case=c, arm=a) for a in arms]
            fac.append([c[8:], '%d' % float(r['note']) if r['note'] else '', ms(v[0]), ms(v[1]), ms(v[2]), times(v[0], v[2])])
    fl = [[r['arm'], ms(float(r['value']))] for r in rows if r['case'].startswith('fields: ')]
    sz = [[r['arm'].replace('orders_', ''), mib(float(r['value']))] for r in rows if r['case'] == 'index']
    lines = intro(meta, rows, 'Four facets of one search (top 10 categories, top 10 countries, two price buckets, the month) in one `sift.facet_counts` call against four `GROUP BY` statements, in milliseconds.')
    lines += table(['search', 'matches', '`facet_counts`', 'four GROUP BYs, `SiftAgg`', 'four GROUP BYs, normal plan', '`facet_counts` vs normal plan'], fac, ['---', '---:', '---:', '---:', '---:', '---:'])
    lines += ['', 'Top 10 of a two-word search by BM25, one field against two fields with equal and with unequal weights (`sift.field_query`), median ms:', '']
    lines += table(['query', 'ms'], fl, ['---', '---:']) + ['', 'Index sizes (MiB): ' + ', '.join('%s %s' % (a, b) for a, b in sz) + '.']
    return lines + ['', 'Reproduce: `bench/bench extras fields` (results: [results/fields.tsv](results/fields.tsv)).']


# ---- build and write cost ---------------------------------------------------------------------------------------------------------------

def buildcost_section(meta, rows):
    builds = []
    for r in rows:
        if r['arm'] not in builds:
            builds.append(r['arm'])
    base, rest = builds[0], builds[1:]
    body, cells = [], []
    for r in rows:
        if r['case'] not in cells:
            cells.append(r['case'])
    for c in cells:
        b = get(rows, case=c, arm=base)
        row = [c, ms(float(b['value'])) if b else 'n/a']
        for a in rest:
            r = get(rows, case=c, arm=a)
            row.append('n/a' if r is None else '%s [%s, %s] %s' % (r['value'], r['min'], r['max'], r['note']))
        body.append(row)
    lines = intro(meta, rows, 'User-space instructions of the server processes per operation, %s as the baseline (millions of instructions per operation); the other columns are the ratio to it with the 95%% interval and the verdict. A ratio below 1 is fewer instructions.' % base)
    return lines + table(['cell', '%s Minstr/op' % base] + rest, body, ['---', '---:'] + ['---:'] * len(rest)) + ['', 'Reproduce: `bench/bench extras buildcost` (results: [results/buildcost.tsv](results/buildcost.tsv)).']


def gate(a):
    """the table of cpu_gate.sh's log -> rows: case = cell, arm = build, metric ratio (min, max the interval, n, note the verdict); the baseline's instructions per operation as metric instr_per_op"""
    vers = dict(kv.split('=', 1) for kv in a.versions.split(','))
    unit = {'G': 1e3, 'M': 1.0, 'k': 1e-3}
    rows = []
    for line in open(a.log):
        m = re.match(r'^(\S+)\s+([0-9.]+)([GMk]?) \|(.*)$', line.rstrip('\n'))
        if not m or m.group(1) in ('cell',):
            continue
        cell, base_val, u, rest = m.group(1), float(m.group(2)), m.group(3), m.group(4)
        rows.append(dict(version='gate', case=cell, arm='v302', metric='Minstr_per_op', value='%.4f' % (base_val * unit.get(u, 1e-6)), note=vers.get('v302', '')))
        for seg in rest.split('|'):
            g = re.match(r'\s*(\S+)\s+([0-9.]+) \[\s*([0-9.]+),\s*([0-9.]+)\]\s+([0-9.]+)\s+(\d+)\s+([0-9.]+)\s*(.*?)\s*$', seg)
            if g:
                rows.append(dict(version='gate', case=cell, arm=g.group(1), metric='ratio', value=g.group(2), min=g.group(3), max=g.group(4), n=g.group(6), note=g.group(8) + ' (%s)' % vers.get(g.group(1), '')))
    with open(a.rows, 'a') as f:
        for r in rows:
            f.write('\t'.join(str(r.get(c, '')) for c in COLUMNS) + '\n')


# ---- GPU --------------------------------------------------------------------------------------------------------------------------------

def gpu_section(meta, rows):
    qs = dict(re.findall(r'^\t"(\w+)\|([^"]+)"$', open(os.path.join(REPO, 'bench', 'gpu', 'bench-pg.sh')).read(), re.M))
    body = []
    for r in rows:
        if r['mode'] != 'cpu_1core':
            continue
        g = next((x for x in rows if x['rows'] == r['rows'] and x['query'] == r['query'] and x['mode'] == 'gpu_cold_resident'), None)
        if g is None:
            continue
        c, d = float(r['median_ms']), float(g['median_ms'])
        body.append(['{:,}'.format(int(r['rows'])), '`%s`' % r['query'], ms(float(r['first_ms'])), ms(c), ms(float(g['first_ms'])), ms(d), times(d, c)])
    lines = [meta.get('title', '').lstrip('# '), '',
             'Queries on the table of the online-store benchmark, with one sift_col index over the columns they read: ' + '; '.join('`%s` `%s`' % (k, v) for k, v in qs.items()) + '.', '']
    lines += table(['rows', 'query', 'CPU, first (ms)', 'CPU, median (ms)', 'device, first (ms)', 'device, median, resident (ms)', 'resident vs CPU'], body, ['---', '---', '---:', '---:', '---:', '---:', '---:'])
    return lines + ['', 'Reproduce: `bench/bench extras gpu` (a Vulkan device and `VULKAN=1`; results: [results/gpu.tsv](results/gpu.tsv)).']




def wrap(a):
    """rows of a runner (no header) -> a results file: title, the run's meta, the host load at the start and now"""
    meta = dict(m.split('=', 1) for m in a.meta or [])
    meta['host_load_before'] = a.load_before
    meta['host_load_after'] = host_load()
    rows = [dict(zip(COLUMNS, line.rstrip('\n').split('\t'))) for line in open(a.rows) if line.strip()]
    write(a.out, a.title, meta, rows)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('cmd', choices=['render', 'wrap', 'gate'])
    ap.add_argument('--name')
    ap.add_argument('--title')
    ap.add_argument('--rows')
    ap.add_argument('--out')
    ap.add_argument('--load-before', default='')
    ap.add_argument('--meta', action='append')
    ap.add_argument('--log')
    ap.add_argument('--raw')
    ap.add_argument('--versions')
    ap.add_argument('--results', default=os.path.join(REPO, 'docs', 'results'))
    ap.add_argument('--benchmarks', default=os.path.join(REPO, 'docs', 'BENCHMARKS.md'))
    ap.add_argument('--readme', default=os.path.join(REPO, 'README.md'))
    a = ap.parse_args()
    if a.cmd == 'wrap':
        return wrap(a)
    if a.cmd == 'gate':
        return gate(a)
    for name, section in SECTIONS.items():
        path = os.path.join(a.results, name + '.tsv')
        if os.path.exists(path):
            meta, rows = read(path)
            lines = section(meta, rows)
            replace_block(a.benchmarks, name, lines)
            for doc in ALSO.get(name, []):   # the other documents that carry the block
                if os.path.exists(doc) and '<!-- bench:%s:begin' % name in open(doc).read():
                    replace_block(doc, name, [l.replace('](results/', '](docs/results/') for l in lines] if os.path.basename(doc) == 'README.md' else lines)


ALSO = {'gpu': [os.path.join(REPO, 'README.md'), os.path.join(REPO, 'docs', 'GPU.md')]}
SECTIONS = {'sets': sets_section, 'rollup': rollup_section, 'tablewrites': tablewrites_section, 'writepath': writepath_section, 'bm25size': bm25size_section,
            'leaftypes': leaftypes_section, 'fields': fields_section, 'buildcost': buildcost_section, 'gpu': gpu_section}

if __name__ == '__main__':
    main()
