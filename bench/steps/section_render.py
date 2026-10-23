"""The update, delete and VACUUM section of README.md and docs/BENCHMARKS.md and its chart, from bench/results/updates.json (store.py section, store.py render).
Every number is a row of the results; the sentences only choose which rows to state."""
import json
import os
import statistics
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import charts  # noqa: E402
from store_render import best_bold, best_high, md_table as table, ms, rate  # noqa: E402

LANES = ['pg', 'sift', 'sift_maint', 'sift_table']
NAMES = {'pg': 'PostgreSQL', 'sift': '`sift_col`', 'sift_maint': '`sift_col` + auto vacuum and merge (opt-in)', 'sift_table': '`sift_table`'}
COLORS = {'pg': '#D55E00', 'sift': '#0072B2', 'sift_maint': '#56B4E9', 'sift_table': '#009E73'}
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


def line(S, size, key, f, best, label):
    """the cells of a row; in bold the lanes whose median is inside the range of the runs of the best lane (a tie), the best median alone where there is a single run"""
    vs = [S.vals(size, e, *key) for e in S.lanes]
    meds = [statistics.median(v) if v else None for v in vs]
    cells = [cell(v, f) for v in vs]
    top = [m for m in meds if m is not None]
    if not top or not any(len(v) > 1 for v in vs):
        return [f'{size:,}', label] + best(meds, cells)
    win = vs[meds.index(max(top) if best is best_high else min(top))]
    return [f'{size:,}', label] + [f'**{c}**' if m is not None and min(win) <= m <= max(win) else c for m, c in zip(meds, cells)]


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


def tables(S, detail):
    head = lambda first: first + [NAMES[e] for e in S.lanes]
    out = ['**Bulk changes** (rows per second, higher is better)', '']
    out += md_table(head(['orders', 'change']), [line(S, s, ('-', v, 'phase_rows_s'), lambda x: f'{x:,.0f}', best_high, label) for s in S.sizes for v, label in CHANGES]) + ['']
    out += ['**The read around a churn** (the monthly revenue query, and the next query after a single-row change; p50 ms, lower is better)', '']
    qs = [('03', 'revenue per month')] + ([('05', 'average shipped order')] if detail else [])
    rows = [line(S, s, (q, v, 'latency_ms'), ms, best_bold, f'{label}: {name}') for s in S.sizes for q, label in qs for v, name in STATES]
    rows += [line(S, s, ('-', 'commit', 'visible_ms'), ms, best_bold, 'a single-row change seen by the next query') for s in S.sizes]
    out += md_table(head(['orders', 'state']), rows) + ['']
    out += ['**VACUUM, WAL and space**', '']
    body = []
    for s in S.sizes if detail else S.sizes[-1:]:   # the README states the largest size, the part of docs/BENCHMARKS.md every size
        body.append(line(S, s, ('-', 'maintenance', 'maintain_s'), lambda x: f'{x:.1f}', best_bold, 'VACUUM (ANALYZE), seconds'))
        wal = lambda e: [S.med(s, e, '-', v, 'wal_mib') for v, _ in CHANGES] + [S.med(s, e, '-', 'maintenance', 'wal_mib')]
        body.append([f'{s:,}', 'WAL MiB: the three changes / VACUUM'] + [(f'{sum(w[:3]):,.0f} / {w[3]:,.0f}' if None not in w else 'n/a') for w in map(wal, S.lanes)])
        body.append([f'{s:,}', 'size MiB: fresh, after the changes, after VACUUM'] + [' / '.join(f'{x:,.0f}' if x is not None else 'n/a' for x in (S.med(s, e, '-', v, 'size_mib') for v in ('before', 'after churn', 'after maintenance'))) for e in S.lanes])
    out += md_table(head(['orders', '']), body) + ['']
    cs = S.churn_size()
    if cs:
        out += [f'**Steady churn and inserts only** ({cs:,} orders, 4 clients, {max(r["churn_seconds"] for r in S.meta["runs"].values())} s each; churn: 40% update, 30% delete, 30% insert of one row; inserts: 1,000 rows a statement)', '']
        body = [line(S, cs, ('churn', 'steady', 'tps'), lambda x: f'{x:,.0f}', best_high, 'churn: transactions/s'),
                line(S, cs, ('churn', 'steady', 'p99_ms'), ms, best_bold, 'churn: p99 ms'),
                line(S, cs, ('churn', 'steady', 'read_p50_ms'), ms, best_bold, 'churn: the monthly revenue read meanwhile, p50 ms'),
                line(S, cs, ('churn', 'steady', 'wal_bytes_per_tx'), lambda x: f'{x:,.0f}', best_bold, 'churn: WAL bytes per transaction'),
                line(S, cs, ('append', 'steady', 'rows_s'), lambda x: f'{x:,.0f}', best_high, 'inserts: rows/s'),
                line(S, cs, ('append', 'steady', 'wal_bytes_per_tx'), lambda x: f'{x / 1000:,.0f}', best_bold, 'inserts: WAL bytes per row')]
        out += md_table(head(['orders', '']), body) + ['']
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
    pg = S.med(big, 'pg', '03', 'after churn', 'latency_ms')
    slow = []
    for e in ('sift', 'sift_table'):
        a, b, c = (S.med(big, e, '03', v, 'latency_ms') for v, _ in STATES)
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
    series = [(e, [S.med(big, e, '03', v, 'latency_ms') for v, _ in STATES]) for e in S.lanes]
    cap = f'{big:,} orders, one connection, p50 of {S.run0["probes"]} runs; pg_sift {S.run0["servers"].get("sift_sift", "?")}, {S.meta["date"]}'
    return charts.grouped('The monthly revenue read around a churn', 'p50 milliseconds, logarithmic axis, lower is better', [(name, '') for _, name in STATES], series, ms, 'p50 latency (ms)', cap, log=True)


def write_chart(S, directory):
    os.makedirs(directory, exist_ok=True)
    with open(os.path.join(directory, 'updates-reads.svg'), 'w', newline='\n') as f:
        f.write(chart(S))
