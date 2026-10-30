"""The update, delete and VACUUM section of README.md and docs/BENCHMARKS.md and its chart, from bench/results/updates.json (store.py section, store.py render).
Every number is a row of the results; the sentences only choose which rows to state."""
import json
import os
import statistics
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import charts  # noqa: E402
import mdtable  # noqa: E402
import triple  # noqa: E402
from store_render import best_bold, best_high, md_table as table, ms, rate  # noqa: E402

LANES = ['pg', 'sift', 'sift_maint', 'sift_table']
NAMES = {'pg': 'PostgreSQL', 'sift': '`sift_col`', 'sift_maint': '`sift_col` + auto vacuum and merge (opt-in)', 'sift_table': '`sift_table`'}
COLORS = {'pg': '#D55E00', 'sift': '#0072B2', 'sift_maint': '#56B4E9', 'sift_table': '#009E73'}
SECONDS = lambda v: v * 1000   # noqa: E731
PROBE = triple.NA('a stopwatch probe from the change to the next query that sees it, not a statement the server accounts')
CHANGES = [('update 1%', 'update 1% of the rows'), ('update 10%', 'update 10% of the rows'), ('delete 5%', 'delete 5% of the rows')]
STATES = [('baseline', 'fresh table'), ('after churn', 'right after the changes'), ('after maintenance', 'after VACUUM')]
# why a cell loses, one line each; a sentence is printed only for a cell that loses in the results
CAUSE = {'visible': 'the next query finds the unit of the changed row dirty and checks its rows against the heap',
         'bulk': 'a changed row also marks its unit of the column index dirty, which a PostgreSQL heap has no equivalent of',
         'reads': 'until VACUUM (or a merge) a unit with a changed row is not answered from its pre-aggregates and its rows are checked against the heap'}


md_table = lambda head, body: table(head, body, ['---', '---'] + ['---:'] * (len(head) - 2))


class Section:
    def __init__(self, path):
        d = json.load(open(path))
        self.meta, self.rows = d['meta'], d['rows']
        self.sizes = sorted({int(t.split('-')[0]) for t in self.meta['runs']})
        self.lanes = [e for e in LANES if any(r['engine'] == e for r in self.rows)]
        self.run0 = next(iter(self.meta['runs'].values()))

    def vals(self, size, lane, cls, variant, metric):
        return [r['value'] for r in self.rows if r['tier'].startswith(f'{size}-r') and r['engine'] == lane and r['class'] == cls and r['variant'] == variant and r['metric'] == metric
                and isinstance(r['value'], (int, float))]

    def med(self, *key):
        v = self.vals(*key)
        return statistics.median(v) if v else None

    def reps(self, size):
        return len({t for t in self.meta['runs'] if t.startswith(f'{size}-r')})

    def churn_size(self):
        return next((s for s in self.sizes if any(r['class'] == 'churn' and r['tier'].startswith(f'{s}-r') for r in self.rows)), None)


def cell(v, f):
    """a median with the range of the repetitions where there are several"""
    if not v:
        return 'n/a'
    m = f(statistics.median(v))
    return m if len(v) == 1 else f'{m} [{f(min(v))}-{f(max(v))}]'


def line(S, size, key, f, best):
    """the cells of a column, one for each lane; in bold the lanes whose median is inside the range of the runs of the best lane (a tie), the best median alone where there is a single run"""
    vs = [S.vals(size, e, *key) for e in S.lanes]
    meds = [statistics.median(v) if v else None for v in vs]
    cells = [cell(v, f) for v in vs]
    top = [m for m in meds if m is not None]
    if not top or not any(len(v) > 1 for v in vs):
        return best(meds, cells)
    win = vs[meds.index(max(top) if best is best_high else min(top))]
    return [f'**{c}**' if m is not None and min(win) <= m <= max(win) else c for m, c in zip(meds, cells)]


def header(S):
    sv = S.run0['servers']
    loads = [float(r['loadavg'][0]) for r in S.meta['runs'].values()]
    noisy = sum(len(r.get('noisy') or []) for r in S.meta['runs'].values())
    churn = [r['churn_seconds'] for r in S.meta['runs'].values() if r['churn_seconds']]
    reps = ', '.join(f'{s:,} orders: ' + (f'median of {S.reps(s)} runs [min-max]' if S.reps(s) > 1 else 'one run') for s in S.sizes)
    setting = lambda e: f'`{sv[e + "_maintenance"]}`' if sv.get(e + '_maintenance') else 'the settings of the server'
    sha = f' ({S.meta["sha"]})' if S.meta.get('sha') else ''
    return [f'Measured at pg_sift {sv.get("sift_sift", "?")}{sha} on {S.meta["date"]}, {S.meta["host"].get("cpu", "?")} ({S.meta["host"].get("threads", "?")} threads), PostgreSQL {sv.get("pg", "?")}, '
            f'one container of {S.meta["cpus"]} CPUs; load average {min(loads):.1f}-{max(loads):.1f} at the start of the runs, {noisy} NOISY window{"s" * (noisy != 1)}. {reps}.',
            f'`sift_col` and `sift_table` run the defaults of that build ({setting("sift")}); the opt-in column is the `sift_col` layout in a database with {setting("sift_maint")} (all read back from the server). '
            'In bold: the best median of a row and the medians inside the range of its runs.']


def fig(S, size, lane, cls, variant, time, cpu, io):
    """(ms, CPU ms, MB) of a lane from the medians of its metrics, each a name or (name, conversion); None when the lane has no time"""
    def get(spec):
        if isinstance(spec, triple.NA):
            return spec
        name, convert = (spec, float) if isinstance(spec, str) else spec
        v = S.med(size, lane, cls, variant, name)
        return None if v is None else convert(v)

    t = get(time)
    return None if t is None else (t, *(v if v is not None else triple.NA(triple.NOT_MEASURED) for v in map(get, (cpu, io))))


def triples(S, size, columns, **kw):
    """the lines of the table of the lanes against `columns` [(header, (class, variant, time, cpu, io))], and the figures shown (for the legend)"""
    cols = [(h, h) for h, _ in columns]
    spec = dict(columns)
    figs = lambda e, h: fig(S, size, e, spec[h][0], spec[h][1], *spec[h][2:])  # noqa: E731
    ref = lambda e, h: triple.best([figs(m, h) for m in S.lanes if m in triple.MINE]) or (None,) * 3
    return triple.rows({NAMES[e]: e for e in S.lanes}, cols, figs, ref, **kw), [f for e in S.lanes for h, _ in cols if (f := figs(e, h))]


def tables(S, detail):
    """a table for each question, a row for each lane and a cell `time · CPU · MB` for each measure (steps/triple.py); the legend closes them"""
    out, shown = [], []

    def emit(caption, size, columns, **kw):
        lines, figs = triples(S, size, columns, **kw)
        out.extend([caption, ''] + lines + [''])
        shown.extend(figs)

    for s in S.sizes:
        emit(f'**Bulk changes**, {s:,} orders (milliseconds to change the rows, the CPU of the server meanwhile, MB of WAL written; lower is better)',
             s, [(h, ('-', v, (('phase_s', SECONDS)), 'phase_cpu_ms', 'wal_mib')) for v, h in CHANGES])
    qs = [('revenue-per-month', 'revenue per month')] + ([('average-shipped-order', 'average shipped order')] if detail else [])
    for s in S.sizes:
        emit(f'**The read around a churn**, {s:,} orders (the monthly revenue query' + (' and the average shipped order' if detail else '') + ': p50 milliseconds, CPU and MB read of a request; lower is better)', s,
             [(f'{label}: {name}' if detail else name, (q, v, 'latency_ms', 'cpu_ms', 'read_mb')) for q, label in qs for v, name in STATES]
             + [('a single-row change seen by the next query', ('-', 'commit', 'visible_ms', PROBE, PROBE))])
    for s in S.sizes if detail else S.sizes[-1:]:   # the README states the largest size, the part of docs/BENCHMARKS.md every size
        emit(f'**VACUUM (ANALYZE)**, {s:,} orders (milliseconds, CPU, MB of WAL)', s, [('VACUUM', ('-', 'maintenance', ('maintain_s', SECONDS), 'maintain_cpu_ms', 'wal_mib'))])
    cs = S.churn_size()
    if cs:
        emit(f'**Steady churn and inserts only** ({cs:,} orders, {S.run0['clients']} clients, {max(r["churn_seconds"] for r in S.meta["runs"].values())} s each; churn: 40% update, 30% delete, 30% insert of one row; inserts: 1,000 rows a statement; '
             'per 1,000 transactions: the wall milliseconds, the CPU of the server and the MB of WAL)', cs,
             [('churn', ('churn', 'steady', ('tps', lambda v: 1e6 / v), ('cpu_ms_per_tx', lambda v: v * 1000), ('wal_bytes_per_tx', lambda v: v * 1000 / 2**20))), ('inserts', ('append', 'steady', ('tps', lambda v: 1e6 / v), ('cpu_ms_per_tx', lambda v: v * 1000), ('wal_bytes_per_tx', lambda v: v * 1000 / 2**20)))])
    out += triple.legend(shown, 'MB (written to the WAL by a change, read by a request)') + ['']
    space = lambda e: [S.med(s, e, '-', v, 'size_mib') for s in S.sizes[-1:] for v in ('before', 'after churn', 'after maintenance')]
    out += [f'**Space**, {S.sizes[-1]:,} orders (MiB: fresh / after the changes / after VACUUM)', ''] + md_table(['system', 'size MiB'], [[NAMES[e], ' / '.join(f'{x:,.0f}' if x is not None else 'n/a' for x in space(e))] for e in S.lanes]) + ['']
    if cs:
        out += [f'**Latency tails under the churn** ({cs:,} orders; p99 of a churn transaction and p50 of the monthly revenue read while it runs, milliseconds)', ''] + md_table(
            ['system', 'churn p99', 'read p50 under churn'], [[NAMES[e], ms(S.med(cs, e, 'churn', 'steady', 'p99_ms')) if S.med(cs, e, 'churn', 'steady', 'p99_ms') else 'n/a',
                                                              ms(S.med(cs, e, 'churn', 'steady', 'read_p50_ms')) if S.med(cs, e, 'churn', 'steady', 'read_p50_ms') else 'n/a'] for e in S.lanes]) + ['']
    return out


def losses(S):
    """one sentence per kind of loss, each only when the results have it"""
    out, big = [], S.sizes[-1]
    for s in S.sizes:
        for v, label in CHANGES:
            pg = S.med(s, 'pg', '-', v, 'phase_rows_s')
            mine = {e: S.med(s, e, '-', v, 'phase_rows_s') for e in ('sift', 'sift_table')}
            mine = {e: x for e, x in mine.items() if x}
            if pg and mine and pg > max(mine.values()) * 1.05:
                best = max(mine, key=mine.get)
                out.append(f'{label} at {s:,} orders: PostgreSQL {pg / mine[best]:.1f}x faster than {NAMES[best]} ({rate(pg)} against {rate(mine[best])} rows/s); {CAUSE["bulk"]}.')
    pg = S.med(big, 'pg', 'revenue-per-month', 'after churn', 'latency_ms')
    slow = []
    for e in ('sift', 'sift_table'):
        a, b, c = (S.med(big, e, 'revenue-per-month', v, 'latency_ms') for v, _ in STATES)
        if a and b and b > 10 * a:
            slow.append(f'{NAMES[e]} {ms(b)} ms ({b / a:.0f}x its fresh {ms(a)} ms; {ms(c)} ms after VACUUM)')
    if slow:
        out.append(f'the monthly revenue read at {big:,} orders right after the changes is much slower than on the fresh table: ' + ', '.join(slow) + (f', PostgreSQL {ms(pg)} ms' if pg else '') + f'; {CAUSE["reads"]}.')
    vis = {e: S.med(big, e, '-', 'commit', 'visible_ms') for e in ('pg', 'sift', 'sift_table')}
    if all(vis.values()) and max(vis['sift'], vis['sift_table']) > 2 * vis['pg']:
        out.append(f'a single-row change is seen by the next query after {ms(vis["sift"])} ms on `sift_col` and {ms(vis["sift_table"])} ms on `sift_table`, PostgreSQL {ms(vis["pg"])} ms; {CAUSE["visible"]}.')
    return out


def readme_block(S):
    out = header(S) + [''] + tables(S, False) + ['![The monthly revenue read around a churn](docs/img/updates-reads.svg)', '']
    ls = losses(S)
    out += ['**What is slower, and why**', ''] + ['- ' + x for x in ls] if ls else ['No cell of this pass is slower than PostgreSQL, and no read is slowed more than tenfold by the changes.']
    out += ['']
    out += ['- Method, the steps, every run and the comparison with Elasticsearch and ClickHouse: [docs/BENCHMARKS.md](docs/BENCHMARKS.md#updates-and-deletes).']
    return out


def benchmarks_block(S):
    out = header(S) + ['', 'Steps (`bench/steps/store_updates.py`, `bench/steps/store_section.py`): update 1% (`id % 100 = 0`), update 10% (`id % 10 = 1`), delete 5% (`id % 20 = 3`) in statements of 1,000 ids over one connection, '
                       'the reads before, right after and after `VACUUM (ANALYZE)`, then the steady churn and the inserts only on a freshly loaded table of each lane.', '']
    noise = [[t, r['loadavg'][0], f'{r["churn_seconds"]} s' if r['churn_seconds'] else 'none', ', '.join(sorted(n.split(':')[0].split(' ')[-1] for n in r.get('noisy') or [])) or 'none'] for t, r in S.meta['runs'].items()]
    out += tables(S, True) + ['**The runs** (load average at the start; the lanes whose window was NOISY: other work of the host above its idle level)', '']
    out += md_table(['run', 'load average', 'churn windows', 'NOISY lanes'], noise) + ['']
    return out + ['Every run is in `bench/results/updates.json`; the cells above are the median of the runs with their range.']


def chart(S):
    big = S.sizes[-1]
    charts.ENGINES.update({e: (NAMES[e].replace('`', ''), COLORS[e]) for e in S.lanes})
    series = [(e, [S.med(big, e, 'revenue-per-month', v, 'latency_ms') for v, _ in STATES]) for e in S.lanes]
    cap = f'{big:,} orders, one connection, p50 of {S.run0["probes"]} runs; pg_sift {S.run0["servers"].get("sift_sift", "?")}, {S.meta["date"]}'
    return charts.grouped('The monthly revenue read around a churn', 'p50 milliseconds, logarithmic axis, lower is better', [(name, '') for _, name in STATES], series, ms, 'p50 latency (ms)', cap, log=True)


def write_chart(S, directory):
    os.makedirs(directory, exist_ok=True)
    with open(os.path.join(directory, 'updates-reads.svg'), 'w', newline='\n') as f:
        f.write(chart(S))
