#!/usr/bin/env python3
"""The generated blocks of docs/perf/AGGREGATES.md and the Scale block of README.md, from bench/results, standard library only.

    python3 -I bench/steps/aggregates_page.py [--results DIR]

  README.md                  <!-- bench:scale:begin -->       the charts of scale (rows, CPUs); <!-- bench:shapes:begin --> the real query shapes
  docs/perf/AGGREGATES.md    <!-- bench:agg-map:begin -->     every results file behind an aggregate benchmark: what it answers, when, on which host
                             <!-- bench:agg-charts:begin -->  every chart with the question it answers
                             <!-- bench:agg-scale:begin -->   the tables behind the scale charts
                             <!-- bench:agg-shapes:begin -->  the real-world shapes (joins, lookups, top-N, distinct, time buckets, UNION ALL arms)

A block whose results file does not exist says it is not measured yet and names the command; nothing is typed by hand.
"""
import argparse
import json
import os
import statistics
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import bench_charts  # noqa: E402  (scale_takeaway)
import extras  # noqa: E402,F401  (read, duck_table: the pg_duckdb tables)
import readme  # noqa: E402  (replace_block)
import store_charts  # noqa: E402
from store_charts import SYSTEMS, picture  # noqa: E402

REPO = os.path.dirname(os.path.dirname(HERE))
PAGE = os.path.join(REPO, 'docs', 'perf', 'AGGREGATES.md')
IMG = '../img/'
LABEL = {e: SYSTEMS[e][0] for e in SYSTEMS}


def md(head, body, align=None):
    align = align or ['---'] + ['---:'] * (len(head) - 1)
    return ['| ' + ' | '.join(head) + ' |', '|' + '|'.join(align) + '|'] + ['| ' + ' | '.join(r) + ' |' for r in body]


def ms(v):
    return 'n/a' if v is None else f'{v:,.0f}' if v >= 100 else f'{v:.3g}'


def not_measured(cmd, what):
    return [f'Not measured yet: `{cmd}` writes {what}.']


def maybe(results, name):
    p = os.path.join(results, name)
    return p if os.path.exists(p) else None


# ---- what is measured --------------------------------------------------------------------------------------------------------------------------

CHARTS = [
    ('agg-latency', 'How long does each question take on each system, pg_duckdb included, and how much do the runs vary?', 'store.json'),
    ('agg-speedup', 'How many times faster than PostgreSQL is each system, warm and on the first run after a restart?', 'store.json'),
    ('store-cpus', 'What does a second and third CPU buy each system?', 'store.json'),
    ('store-size', 'What does it cost in space and in load time?', 'store.json'),
    ('cost-per-row', 'What does a row cost in index bytes, load time and update rate?', 'store.json'),
    ('store-updates', 'Does the data grow under updates and deletes, and does maintenance give the space back?', 'store.json'),
    ('scale-growth', 'Does the time grow with the rows, and with what slope?', 'store-scaling.json'),
    ('scale-questions', 'Does the ranking hold from 25,000 to 10,000,000 orders, question by question?', 'store-scaling.json'),
    ('scaling-partitions', 'Does a partitioned table (to 4,096 partitions) keep the aggregates fast, and what does planning cost?', 'scaling.tsv'),
    ('scaling-workers', 'How do the aggregates use parallel workers?', 'scaling.tsv'),
    ('shapes-speedup', 'Do the real query shapes get faster, and does it depend on the size of the filter?', 'analytics.json'),
    ('cases-speedup', 'How much faster is each benchmark case of a query shape than PostgreSQL on the same rows?', 'cases.tsv'),
    ('cases-ecdf', 'On what share of the cases and shapes is pg_sift how many times faster?', 'cases.tsv'),
    ('cases-scale', 'How does each case scale from 100,000 to 3,000,000 rows?', 'cases-scale.tsv'),
    ('cases-workmem', 'Does a large work_mem (4GB) change the ranking?', 'cases-wm4g.tsv'),
]


def charts_lines(results, prefix):
    out = []
    for name, question, src in CHARTS:
        if maybe(results, src):
            out += [f'#### {question}', '', picture(name, question, prefix), '', f'Data: [bench/results/{src}](../results/{src}).' if prefix == IMG else '', '']
    return out


def tsv_meta(path):
    meta, _ = store_charts.read_tsv(path)
    return meta


def map_lines(results):
    rows = []
    s = maybe(results, 'store.json')
    if s:
        S = store_charts.Store(s)
        t = S.tier_of(1_000_000, 3)
        rows.append(['the seven aggregate questions (and the three full-text ones), six systems, 1 and 3 CPUs, warm and cold, updates', 'store.json', S.tiers[t]['parts']['warm']['date'][:10], S.host(), '`bench/bench store all`'])
    sc = maybe(results, 'store-scaling.json')
    if sc:
        S = store_charts.Store(sc)
        sizes = sorted({m['rows'] for m in S.tiers.values()})
        rows.append([f'the aggregate questions at {", ".join(store_charts.rows_label(n) for n in sizes)} orders (the 1M set is in store.json)', 'store-scaling.json', next(iter(S.tiers.values()))['parts']['warm']['date'][:10], S.host(), '`bench/bench store all --sizes N,N`'])
    for f, what, cmd in (('duckdb.tsv', 'pg_duckdb against PostgreSQL and pg_sift, 1,000,000 orders', '`bench/bench duckdb`'), ('duckdb-10000000.tsv', 'the same at 10,000,000 orders', '`bench/bench duckdb --rows 10000000 --out FILE`'), ('scaling.tsv', 'partitions and parallel workers', '`bench/bench scaling`'),
                         ('sets.tsv', 'filters by a set of 1k to 100k ids', '`bench/bench sets`'), ('rollup.tsv', 'rollups of jsonb arrays', '`bench/bench extras rollup`'),
                         ('buildcost.tsv', 'build and write cost per operation', '`bench/bench extras buildcost`')):
        p = maybe(results, f)
        if p:
            m = tsv_meta(p)
            rows.append([what, f, m.get('date', '?'), m.get('cpu', '?').split(' (')[0].replace(' 8-Core Processor', ''), cmd])
    a = maybe(results, 'analytics.json')
    if a:
        meta = json.load(open(a))['meta']
        rows.append(['real-world query shapes: joins, lookups, top-N, distinct, time buckets, `UNION ALL`', 'analytics.json', meta.get('date', '?')[:10], meta.get('host', 'see the file'), '`bench/bench analytics run`'])
    return md(['what it answers', 'results file in bench/results', 'measured', 'host', 'command'], rows, ['---'] * 5)


# ---- scale ---------------------------------------------------------------------------------------------------------------------------------

def rows_tables(results):
    """the winner of every question at every size, and how much each system grew from the smallest to the largest size"""
    S = store_charts.Store(os.path.join(results, 'store.json'), os.path.join(results, 'store-scaling.json'))
    slug = store_charts.question_slugs()
    sizes = sorted({m['rows'] for m in S.tiers.values() if m.get('cpus') == 3})
    tiers = {n: S.tier_of(n, 3) for n in sizes}
    body = []
    for q in S.queries:
        cells = []
        for n in sizes:
            t = tiers[n]
            v = {e: S.lat(t, e, q)['value'] for e in S.engines(t) if S.lat(t, e, q)}
            win = min(v, key=v.get)
            mine = min(v.get('sift', 1e99), v.get('sift_table', 1e99))
            cells.append(f'**{store_charts.SHORT[win]}**' if win in ('sift', 'sift_table') else f'{store_charts.SHORT[win]} ({mine / v[win]:.1f}x ahead of pg_sift)')
        body.append([f'Q{q} {slug[q]}'] + cells)
    out = ['The fastest system per question (warm median, 3 CPUs); in bold when it is pg_sift, otherwise how many times faster than the better layout of pg_sift it is:', '']
    out += md(['question'] + [f'{n:,} orders' for n in sizes], body, ['---'] + ['---'] * len(sizes)) + ['']
    lo, hi = tiers[sizes[0]], tiers[sizes[-1]]
    engines = [e for e in S.engines(hi) if e in S.engines(lo)]
    body = [[f'Q{q} {slug[q]}'] + [f'{S.lat(hi, e, q)["value"] / S.lat(lo, e, q)["value"]:.0f}x' if S.lat(hi, e, q) and S.lat(lo, e, q) else 'n/a' for e in engines] for q in S.queries]
    out += [f'How much longer a question takes at {sizes[-1]:,} orders than at {sizes[0]:,} ({sizes[-1] // sizes[0]:,}x the rows); a system below {sizes[-1] // sizes[0]:,}x grows slower than the data:', '']
    out += md(['question'] + [LABEL[e] for e in engines], body) + ['']
    wins = {n: sum(1 for q in S.queries if (v := {e: S.lat(tiers[n], e, q)['value'] for e in S.engines(tiers[n]) if S.lat(tiers[n], e, q)}) and min(v, key=v.get) in ('sift', 'sift_table')) for n in sizes}
    grow = {e: statistics.median(S.lat(hi, e, q)['value'] / S.lat(lo, e, q)['value'] for q in S.queries if S.lat(hi, e, q) and S.lat(lo, e, q)) for e in engines}
    out += ['**Takeaway:** pg_sift is the fastest system on ' + ', '.join(f'{wins[n]} of {len(S.queries)} questions at {n:,} orders' for n in sizes) + '; the median question takes ' + ', '.join(f'{grow[e]:.0f}x as long on {LABEL[e]}' for e in ('sift', 'pg', 'ch') if e in grow) + f' at {sizes[-1]:,} orders than at {sizes[0]:,}.', '', '**Caveats**', '']
    noisy = [f'at {S.tiers[tiers[n]]["rows"]:,} orders: ' + '; '.join(S.tiers[tiers[n]]['parts']['warm']['noisy']) for n in sizes if S.tiers[tiers[n]]['parts']['warm'].get('noisy')]
    out += [f'- Cells the harness flagged NOISY after measuring them again (the host is shared), {x}.' for x in noisy]
    out += ['- One pass per size, 3 CPUs per container, no cold or restart runs; the 1,000,000-order column is the pass of the tables above. A system that is not the fastest here can still be within noise of it.', '',
            'Raw results: [bench/results/store-scaling.json](../results/store-scaling.json) (25,000 to 10,000,000 orders) and [bench/results/store.json](../results/store.json); reproduce with `bench/bench store all --sizes 25000,100000,10000000 --cold-runs 0 --restart-runs 0 --update-sizes none`.', '']
    return out


def dim_table(results, dim, label):
    meta, rows = store_charts.read_tsv(os.path.join(results, 'scaling.tsv'))
    rows = [r for r in rows if r['case'].startswith(dim + ' ') and r['metric'] == 'median_ms']
    xs = sorted({int(r['case'].split()[1]) for r in rows})
    qs = sorted({r['arm'].split('_')[0] for r in rows})
    slug = store_charts.question_slugs()
    get = lambda x, arm: next((float(r['value']) for r in rows if r['case'] == f'{dim} {x}' and r['arm'] == arm), None)
    plain = lambda x, arm: arm.endswith('_sift') and arm != 'q01_sift' and next((r['note'] for r in rows if r['case'] == f'{dim} {x}' and r['arm'] == arm), 'SiftAgg').split()[0] != 'SiftAgg'
    cell = lambda x, q, sid: ms(get(x, f'{q}_{sid}')) + (' †' if plain(x, f'{q}_{sid}') else '')
    head = [label] + [f'Q{q[1:]} {slug[q[1:]]}: {s}' for q in qs for s in ('PostgreSQL', 'pg_sift')]
    body = [[f'{x:,}'] + [cell(x, q, sid) for q in qs for sid in ('pg', 'sift')] for x in xs]
    bad = [f'{r["case"]} {r["arm"]}' for r in rows if 'DIFFERS' in r['note']]
    agg = [q for q in qs if q != 'q01']
    behind = [x for x in xs if any(get(x, f'{q}_sift') > get(x, f'{q}_pg') * 1.05 for q in agg)]
    ahead = [x for x in xs if x not in behind]
    take = (f'**Takeaway:** pg_sift is faster than PostgreSQL on {", ".join("Q" + q[1:] for q in agg)} at {ahead[0]:,} to {ahead[-1]:,} {label}' if ahead else '**Takeaway:** pg_sift is not faster than PostgreSQL at any value here')
    take += (f'; at {", ".join(f"{x:,}" for x in behind)} {label} it is slower on ' + ', '.join(f'Q{q[1:]} ({ms(get(x, f"{q}_sift"))} against {ms(get(x, f"{q}_pg"))} ms)' for x in behind[-1:] for q in agg if get(x, f'{q}_sift') > get(x, f'{q}_pg') * 1.05) if behind else '') + '.'
    return [f'{int(meta["orders"]):,} orders of the store benchmark ({meta.get("date", "?")}, {meta.get("cpu", "?")}, {meta.get("postgresql", "?")}, pg_sift {meta.get("sift", "?")}), one client, `psql \\timing` with planning, median of {rows[0]["n"]} runs in milliseconds.', ''] + md(head, body) + ['', take, '', '**Caveats**', '',
            ] + (['- † the planner kept the normal plan on the pg_sift arm (no `SiftAgg` node), so the cell is the same plan as PostgreSQL\'s.'] if any(plain(x, f'{q}_sift') for x in xs for q in qs) else []) + [
            '- Q01 (a lookup by id) is pruned to one partition and uses the primary key in both arms.'] + ([f'- RESULT DIFFERS: {", ".join(bad)}.'] if bad else []) + [
            '- A scratch cluster on a shared host with 4 CPUs (`bench/bench scaling`), ' + ('2 parallel workers per query' if dim == 'partitions' else 'one plain table') + '; every pg_sift result was compared with the normal plan\'s.', '',
            'Raw results: [bench/results/scaling.tsv](../results/scaling.tsv); reproduce with `bench/bench scaling --docs`.', '']


def scale_lines(results, prefix, brief=False):
    out = []
    store = maybe(results, 'store.json')
    cpus = bool(store and store_charts.Store(store).tier_of(1_000_000, 1))
    names = [('scale-growth', maybe(results, 'store-scaling.json')), ('scale-questions', maybe(results, 'store-scaling.json')), ('cases-scale', maybe(results, 'cases-scale.tsv')), ('store-cpus', cpus),
             ('scaling-partitions', maybe(results, 'scaling.tsv')), ('scaling-workers', maybe(results, 'scaling.tsv'))]
    if not any(have for _, have in names):
        return not_measured('bench/bench store all --sizes N,N; bench/bench scaling', 'bench/results/store-scaling.json and bench/results/scaling.tsv')
    alt = {'scale-growth': 'Growth of the time of the median question from 25,000 to 10,000,000 orders, with the log-log slope per system', 'scale-questions': 'Time per question at several table sizes, with the slope of each line',
           'cases-scale': 'Time against rows of the query-shape cases, PostgreSQL and pg_sift, with the slope', 'store-cpus': 'Gain from 1 to 3 CPUs per question and system',
           'scaling-partitions': 'Aggregates by number of partitions', 'scaling-workers': 'Aggregates by number of parallel workers'}
    take = bench_charts.scale_takeaway(results) if maybe(results, 'store-scaling.json') else None
    for name, have in names:
        if have and (not brief or name != 'scale-questions' and not name.startswith('scaling-')):
            out += [picture(name, alt[name], prefix), '']
            if name == 'scale-growth' and take and brief:
                out += [take + '.', '']
    if brief:
        return out + ['The tables behind these charts, the partition and worker sweeps and the chart of every question: [docs/perf/AGGREGATES.md](docs/perf/AGGREGATES.md#scale-rows-partitions-workers-cpus).']
    if maybe(results, 'store-scaling.json'):
        out += ['### Rows', ''] + rows_tables(results)
    if maybe(results, 'scaling.tsv'):
        out += ['### Partitions', ''] + dim_table(results, 'partitions', 'partitions') + ['### Parallel workers', ''] + dim_table(results, 'workers', 'workers')
    return out + duckdb_scale_lines(results)


def duckdb_scale_lines(results):
    """pg_duckdb against PostgreSQL and pg_sift at the largest size it was run (bench/results/duckdb-N.tsv): one line, the table, the takeaway, the caveats"""
    path = maybe(results, 'duckdb-10000000.tsv')
    if not path:
        return []
    import extras
    meta, rows = extras.read(path)
    cases = extras.duckdb_cases(rows)
    best = extras.duck_summary(rows, cases, 'warm_ms')
    return ['### Against pg_duckdb at 10,000,000 orders', '',
            f'{int(meta["orders"]):,} orders of the store benchmark, one client (`psql` with `\\timing`), median of 3 warm runs in milliseconds, lower is better, the fastest of a row and anything within 5% of it in bold. {extras.footer(meta)} '
            f'pg_duckdb {meta.get("pg_duckdb", "?").split(",")[0]}, {meta.get("duckdb", "?")}; 3 CPUs and 8g per server.', ''] + extras.duck_table(rows, cases, 'warm_ms') + [
            '', '**Takeaway:** ' + best[0][2:].replace('Fastest of the four: ', 'fastest system: ') + ' ' + best[2][2:], '', '**Caveats**', '',
            '- ' + best[1][2:],
            '- † the full-text questions run in DuckDB as a substring match, not an index; the client is `psql`, so the numbers are not those of the tables above.',
            '', 'Raw results: [bench/results/duckdb-10000000.tsv](../results/duckdb-10000000.tsv); reproduce with `bench/bench duckdb --rows 10000000 --out FILE`.', '']


# ---- shapes ----------------------------------------------------------------------------------------------------------------------------

SHAPES_CMD = 'bench/bench analytics run --engines heap,col,flat,ch --flat --clickhouse --docs'


def shape_cases(results):
    """(meta, [(query, set, {engine: wall ms})]) of bench/results/analytics.json; None when the file or its sift_col arm is missing"""
    path = maybe(results, 'analytics.json')
    if not path:
        return None
    d = json.load(open(path))
    rows = {(r['engine'], r['class'], r['variant']): r['value'] for r in d['rows'] if r['metric'] == 'wall_ms'}
    if not any(k[0] == 'col' for k in rows):
        return None
    cases = sorted({(k[1], k[2]) for k in rows}, key=lambda c: (c[0][0] != 'G', c[0], c[1]))
    return d['meta'], [(q, v, {e: rows.get((e, q, v)) for e in ('heap', 'col', 'flat', 'ch')}) for q, v in cases]


def shapes_lines(results):
    found = shape_cases(results)
    if not found:
        return not_measured(SHAPES_CMD, 'bench/results/analytics.json with the sift_col arm')
    meta, cases = found
    desc = {}
    qdir = os.path.join(REPO, 'bench', 'sql', 'analytics', 'queries')
    for f in sorted(os.listdir(qdir)):
        first = open(os.path.join(qdir, f)).readline()
        desc[f[:-4]] = first.split(':', 1)[1].strip() if ':' in first else first
    body, gains = [], []
    for q, v, t in cases:
        gain = t['heap'] / t['col'] if t['heap'] and t['col'] else None
        gains += [gain] if gain else []
        body.append([q, desc.get(q, '')[:90], v if v != '-' else '', ms(t['heap']), ms(t['col']), ms(t['flat']), ms(t['ch']), f'{gain:.1f}x' if gain else 'n/a'])
    out = [f'{meta.get("tier", "?")} orders, median of {meta.get("runs", "?")} runs, {meta.get("date", "?")[:10]}, one backend, `max_parallel_workers_per_gather = 0`; wall milliseconds. PostgreSQL is the heap with the application\'s query, '
           f'pg_sift the same query and heap with the `sift_col` index, flat the best conventional layout (flat tables), ClickHouse a throwaway server. `K10`, `K400`, `K4000` are the number of tenants in the filter.', '']
    out += md(['shape', 'what it does', 'tenants', 'PostgreSQL', 'pg_sift', 'PostgreSQL flat tables', 'ClickHouse', 'pg_sift against PostgreSQL'], body, ['---', '---', '---', '---:', '---:', '---:', '---:', '---:'])
    out += ['', f'pg_sift is faster than the heap on {sum(1 for g in gains if g > 1.05)} of {len(gains)} cases (median {statistics.median(gains):.1f}x).']
    return out


def readme_shapes(results):
    """the README's real query shapes: what they are, how many are faster and by how much, the chart by the size of the filter, and the same cases at a large work_mem"""
    found = shape_cases(results)
    if not found:
        return not_measured(SHAPES_CMD, 'bench/results/analytics.json with the sift_col arm')
    meta, cases = found
    gains = [(t['heap'] / t['col'], q, v) for q, v, t in cases if t['heap'] and t['col']]
    faster, slower = [g for g, _, _ in gains if g >= 1.05], [(1 / g, v) for g, _, v in gains if g <= 0.95]
    rng = lambda vals: f'{min(vals):.1f}x to {max(vals):.0f}x' if max(vals) >= 9.5 else f'{min(vals):.1f}x to {max(vals):.1f}x'
    out = [f'{len(gains)} shapes of an analytics application on {meta.get("tier", "?")} orders, one backend: whole-table reports (counts, groups, distinct, percentiles) and reports of a set of tenants (mapping lookups, rollups at three levels, time series at four granularities, '
           f'top 3 per store, notes by role) with 10, 400 and 4,000 tenants in the filter. pg_sift is faster than the PostgreSQL heap on {len(faster)} of them ({rng(faster)}, median {statistics.median(g for g, _, _ in gains):.1f}x)'
           + (f' and slower on {len(slower)} ({rng([r for r, _ in slower])}; {sum(1 for _, v in slower if v == "K10")} of them with the smallest filter set, where the whole query takes 1 to 5 ms)' if slower else '') + '.', '',
           picture('shapes-speedup', 'Speed-up of pg_sift over the PostgreSQL heap on 8 whole-table shapes and 7 shapes at three filter sizes', 'docs/img/'), '']
    ch = [(q, v, t['ch'] / t['col']) for q, v, t in cases if t['ch'] and t['col']]
    flat = [(q, v, t['flat'] / t['col']) for q, v, t in cases if t['flat'] and t['col']]
    name = lambda q, v: q if v == '-' else f'{q} at {v.replace("K", "")} tenants'
    if ch:
        out += [f'Against ClickHouse (one thread, a throwaway server) pg_sift is faster on {sum(1 for *_, r in ch if r >= 1.05)} of {len(ch)} cases and slower on '
                + ', '.join(name(q, v) + f' ({1 / r:.1f}x)' for q, v, r in sorted(ch, key=lambda c: c[2]) if r <= 0.95) + '; against the best conventional layout of the same data in PostgreSQL (flat tables) it is faster on '
                f'{sum(1 for *_, r in flat if r >= 1.05)} of {len(flat)} and slower on {sum(1 for *_, r in flat if r <= 0.95)} ({sum(1 for _, v, r in flat if r <= 0.95 and v == "K10")} of them with the smallest filter set).', '',
                picture('shapes-latency', 'Time per application shape on PostgreSQL, pg_sift, PostgreSQL flat tables and ClickHouse', 'docs/img/'), '']
    if maybe(results, 'cases-wm4g.tsv'):
        import cases as cases_mod
        import cases_render
        reg = cases_mod.read_registry()
        ms_ = lambda path: {f['case'].id: f['native'] / f['sift'] for f in cases_render.collect(extras.read(path)[1], reg) if f['metric'] == 'ms' and f['native'] and f['sift'] and not f['kept']}
        a, b = ms_(os.path.join(results, 'cases.tsv')), ms_(os.path.join(results, 'cases-wm4g.tsv'))
        both = [i for i in b if i in a]
        out += [f'The application-shaped cases of the table above again at a large `work_mem` ({extras.read(os.path.join(results, "cases-wm4g.tsv"))[0].get("work_mem", "4GB")}, the benchmark cluster runs 64MB): the median speed-up of the {len(both)} cases that run on the scan is '
                f'{statistics.median(a[i] for i in both):.1f}x at 64MB and {statistics.median(b[i] for i in both):.1f}x at 4GB.', '', picture('cases-workmem', 'Speed-up of pg_sift over PostgreSQL for the application-shaped cases at the benchmark work_mem and at 4GB', 'docs/img/'), '']
    return out + ['Every shape with its times on PostgreSQL, on pg_sift, on flat tables and on ClickHouse: [docs/perf/AGGREGATES.md](docs/perf/AGGREGATES.md#shapes-of-real-queries); raw results [bench/results/analytics.json](bench/results/analytics.json).']


BLOCKS = {'agg-map': lambda r: map_lines(r), 'agg-charts': lambda r: charts_lines(r, IMG), 'agg-scale': lambda r: scale_lines(r, IMG), 'agg-shapes': lambda r: shapes_lines(r)}


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--results', default=os.path.join(REPO, 'bench', 'results'))
    ap.add_argument('--page', default=PAGE)
    ap.add_argument('--readme', default=os.path.join(REPO, 'README.md'))
    a = ap.parse_args()
    for name, fn in BLOCKS.items():
        readme.replace_block(a.page, name, fn(a.results))
    readme.replace_block(a.readme, 'scale', scale_lines(a.results, 'docs/img/', brief=True))
    readme.replace_block(a.readme, 'shapes', readme_shapes(a.results))


if __name__ == '__main__':
    main()
