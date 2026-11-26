#!/usr/bin/env python3
"""Render the generated blocks of README.md `## Benchmarks`: the hero strip and the seven <details> sections.

    python3 -I bench/steps/f2/readme.py [--root DIR]

A block is `<!-- bench:readme-SLUG:begin/end -->` and holds its whole <details> (summary, the light/dark chart of docs/charts/readme-SLUG-*.svg, the takeaway, a table of at
most 5 columns and 6 rows, the link to docs/benchmarks/PAGE.md). Every figure is a function of docs/results/ (the committed baseline of readme_charts.py and
scope_stock_tables.py without it), so the tables, the takeaways and the charts stay in lockstep; a suite that has no results says so.
"""
import argparse
import html
import math
import os
import sys
from collections import Counter

HERE = os.path.dirname(os.path.abspath(__file__))
STEPS = os.path.dirname(HERE)
sys.path.insert(0, HERE)
sys.path.insert(0, STEPS)
import readme_charts  # noqa: E402
import resultfile  # noqa: E402
import scope_stock_tables  # noqa: E402
import benchmarks  # noqa: E402
from benchmarks import DASH, bold_best, ms, x  # noqa: E402
from charts.theme import fmt_ms  # noqa: E402

REPO = os.path.dirname(os.path.dirname(STEPS))
PAGE = {'analytics': 'analytics', 'pg-queries': 'pg-queries', 'clickbench': 'clickbench', 'search': 'search', 'scopes': 'scopes', 'scalability': 'scalability', 'storage': 'storage-memory'}
ENGINE = {'sift': '`sift_col`', 'ch': 'ClickHouse', 'duckdb': 'DuckDB', 'pg': 'PG 18'}
ANALYTICS_LABEL = {'Count by country': 'Count by country (`GROUP BY`)', 'Revenue / month': 'Revenue per month (`sum`)', 'Top 10 categories': 'Top 10 categories (`LIMIT 10`)',
                   'Avg shipped order': 'Avg shipped order (3 filters)', 'Distinct / month': 'Distinct customers/mo (`DISTINCT`)', 'Big recent orders': 'Big recent orders (range count)'}
SEARCH_LABEL = {'Exact count(*)': 'Exact `count(*)`', 'Phrase "a b"': 'Phrase `"a b"`', 'Boolean a AND b': 'Boolean `a AND b`', 'Filtered top-k': 'Filtered top-k',
                'Boolean a OR b': 'Boolean `a OR b`', 'Joined top-k*': 'Joined top-k\\*'}
COLD_BASELINE = {'sift': (1.03, 5.50), 'es': (3.11, 4.77), 'paradedb': (13.12, 17.08)}   # p50 ms range over the classes of the committed `slim-cold` run
COLD_ENGINE = {'sift': 'sift', 'es': 'elasticsearch', 'paradedb': 'pg_search'}
COLD_NAME = {'sift': '`pg_sift`', 'es': 'Elasticsearch', 'paradedb': 'ParadeDB'}
PG_FLAGSHIP = ('softdel_live_groupby', 'lateral_first_row', 'cte_series_arms', 'jsonb_each_member', 'grouping_sets_report', 'keyset_page')
CLICKBENCH_ARMS = (('pg', 'PG 18'), ('sift_table', '`sift_table`'), ('sift_col', '`sift_col`'), ('ch', 'ClickHouse'), ('ch_proj', 'CH + PROJECTION'), ('duckdb', 'DuckDB'))


def span(vals):
    lo, hi = min(vals), max(vals)
    return x(lo) if x(lo) == x(hi) else f'{x(lo)[:-1]}–{x(hi)}'


def picture(slug, alt):
    base = f'docs/charts/readme-{slug}'
    return ['<picture>', f'  <source media="(prefers-color-scheme: dark)" srcset="{base}-dark.svg">',
            f'  <img alt="{html.escape(alt)}" src="{base}-light.svg" width="860">', '</picture>']


def details(slug, summary, alt, body, opened=False):
    return ['<details open>' if opened else '<details>', f'<summary>{summary}</summary>', '', *picture(slug, alt), '', *body, '',
            f'[Full tables and methodology](docs/benchmarks/{PAGE[slug]}.md)', '', '</details>']


def paragraphs(texts):
    return [line for text in texts for line in (text, '')]


def not_measured(command, results):
    return f'*Not measured for 6.0 yet: `{command}` writes `docs/results/{results}` and populates this section.*'


# ---- hero ---------------------------------------------------------------------------------------------------------------------------------

def hero(data, root):
    ratio, label = max((v['pg'] / v['sift'], label) for label, _, v in data['analytics'])
    bm25 = max(v['sift'] / v['paradedb'] for _, _, v in data['search'])
    sizes = {name: mib for name, mib, _, _ in data['storage_orders']}
    scoped = data['scoped_rollup']
    alt = (f'pg_sift at a glance: {label} {x(ratio)} faster than PostgreSQL 18 on 10M rows; BM25 search up to {x(bm25)} ParadeDB QPS; '
           f'sift_table {x(sizes["PostgreSQL 18 (stock)"] / sizes["pg_sift (sift_table)"])} smaller than PostgreSQL 18; '
           f'{scoped["faster_cells"]} of {scoped["cells"]} scoped cells faster than stock PostgreSQL.')
    return picture('hero', alt)


# ---- analytics ----------------------------------------------------------------------------------------------------------------------------

def analytics(data, root):
    rows = data['analytics']
    won = [v for _, _, v in rows if v['sift'] == min(v.values())]
    lost = [(label, v, min(v, key=v.get)) for label, _, v in rows if v['sift'] != min(v.values())]
    body = [[ANALYTICS_LABEL.get(label, label), *bold_best([(v[e], ms(v[e])) for e in ENGINE], min)] for label, _, v in rows]
    table = resultfile.table(['Workload (10M orders)', '`pg_sift`', 'ClickHouse', 'DuckDB', 'PG 18'], body, ['---'] + ['---:'] * 4)
    leading = {}
    for label, v, w in lost:
        leading.setdefault(w, []).append(f'{label} ({ms(v[w])} vs {ms(v["sift"])})')
    leads = '; '.join(f'{ENGINE[w]} leads ' + ' and '.join(items) for w, items in leading.items())
    take = [f'`sift_col` wins {len(won)} of {len(rows)} queries: {span([v["ch"] / v["sift"] for v in won])} faster than ClickHouse and '
            f'{span([v["pg"] / v["sift"] for v in won])} faster than PG 18 (3 CPUs, 8 GiB, warm medians).' + (f' {leads}.' if lost else ''),
            'ClickHouse runs a plain `MergeTree`; `sift_col` keeps pre-aggregates. Cold runs, DuckDB, `ClickHouse + PROJECTION` and Elasticsearch: full tables.']
    summary = f'<b>Analytical aggregations</b> — <code>sift_col</code> wins {len(won)} of {len(rows)} queries at 10M rows, up to {x(max(v["ch"] / v["sift"] for v in won))} over ClickHouse'
    alt = f'Speed against disk size at 10M rows: sift_col wins {len(won)} of {len(rows)} queries; rivals win only where marked amber.'
    return details('analytics', summary, alt, [*paragraphs(take), *table], opened=True)


# ---- PostgreSQL-specific queries ----------------------------------------------------------------------------------------------------------

def pg_loss(r):
    """where PG 18 wins a case: its shape, ratio and what pg_sift still saves"""
    why = ", where PG's B-tree answers in tens of microseconds and the planner keeps it" if r['against'] == 'b-tree' else ''
    s, p = r['sift']['buffers'], r['pg']['buffers']
    saves = f', while `pg_sift` touches {x(p / s)} fewer blocks: `{s:,.0f}` vs `{p:,.0f}`' if s and p and p > s else ''
    return f'{r["shape"][0].lower()}{r["shape"][1:]} (`{r["case"]}`, {x(r["ratio"])}{why}{saves})'


def pg_queries(data, root):
    rows = data['pg_cases']['rows']
    won, lost = [r for r in rows if r['ratio'] > 1], [r for r in rows if r['ratio'] <= 1]
    lo, hi = x(min(r['ratio'] for r in won)), x(max(r['ratio'] for r in won))
    meta = data['pg_cases']['meta']
    flag = {r['case']: r for r in rows}
    body = [[f'{r["shape"]} (`{r["case"]}`)', r['against'], *bold_best([(r['sift']['ms'], fmt_ms(r['sift']['ms'])), (r['pg']['ms'], fmt_ms(r['pg']['ms']))], min),
             f'**{x(r["ratio"])}**' if r['ratio'] > 1 else x(r['ratio'])] for r in map(flag.get, PG_FLAGSHIP)]
    table = resultfile.table(['PostgreSQL shape', 'Against', '`pg_sift`', 'PG 18', 'Speedup'], body, ['---', '---', '---:', '---:', '---:'])
    take = [f'`pg_sift` wins {len(won)} of {len(rows)} PostgreSQL-native query shapes (`LATERAL`, multi-reader `CTE`, `JSONB` arrays and `jsonb_each`, `GROUPING SETS`, '
            f'window reports, `SEMI`/`ANTI` joins, soft-delete pre-aggregates, `IN`-list lookups) by {lo}–{hi} against stock PG 18 '
            f'({meta["orders"] / 1000:,.0f}k rows, warm median, exact answer equality; the faster of the heap plan and the B-tree).']
    if lost:
        take.append('Where PG 18 wins (amber in the chart): ' + '; '.join(pg_loss(r) for r in lost) + '.')
    summary = f'<b>PostgreSQL-specific queries</b> — <code>pg_sift</code> wins {len(won)} of {len(rows)} SQL shapes against PG 18 ({lo}–{hi})' + (
        f'; PG B-tree/COPY leads {len(lost)}' if lost else '')
    alt = f'Speedup over PG 18 of {len(rows)} PostgreSQL-specific queries by family: pg_sift wins {len(won)} ({lo}–{hi}); PG 18 wins in amber.'
    return details('pg-queries', summary, alt, [*paragraphs(take), *table])


# ---- clickbench ---------------------------------------------------------------------------------------------------------------------------

def geomean(vals):
    return math.exp(sum(math.log(max(v, 0.001)) for v in vals) / len(vals))


def clickbench_body(rows, meta):
    value = lambda metric: {(r['case'], r['arm']): float(r['value']) for r in rows if r['metric'] == metric and r['value'] not in ('', 'NA')}
    warm, cold = value('warm_ms'), value('cold_ms')
    arms = [(a, n) for a, n in CLICKBENCH_ARMS if any(arm == a for _, arm in warm)]
    cases = sorted({c for c, _ in warm}, key=lambda c: int(c[1:]))
    fastest = Counter(min((a for a, _ in arms if (c, a) in warm), key=lambda a: warm[(c, a)]) for c in cases)
    geo = lambda vals, a: ms(geomean([v for (_, arm), v in vals.items() if arm == a])) if any(arm == a for _, arm in vals) else DASH
    equal = lambda a: sum(1 for r in rows if r['arm'] == a and r['metric'] == 'md5_equal' and r['value'] == '1')
    body = [[n, geo(warm, a), geo(cold, a), f'{fastest[a]} of {len(cases)}', f'{equal(a)} / {len(cases)}'] for a, n in sorted(arms, key=lambda an: geomean([v for (_, a), v in warm.items() if a == an[0]]))]
    table = resultfile.table(['Arm', 'Warm geomean', 'Cold geomean', 'Fastest on', '`md5_equal`'], body, ['---'] + ['---:'] * 4)
    best = body[0][0]
    take = (f'Geometric mean of the wall time over {len(cases)} queries on {meta["tier"]} `hits` rows ({meta["date"]}); {best} has the lowest warm geomean. '
            'An arm whose rows differ from PG 18 records no timing.')
    return [take, '', *table], f'<b>ClickBench <code>Q0–Q{len(cases) - 1}</code></b> — {best} has the lowest warm geomean over {len(cases)} queries'


def clickbench(data, root):
    path = os.path.join(root, 'docs', 'results', 'clickbench.tsv')
    if os.path.exists(path):
        meta, rows = resultfile.read(path)
        body, summary = clickbench_body(rows, meta)
    else:
        body, summary = [not_measured('bench/bench clickbench run', 'clickbench.tsv')], '<b>ClickBench <code>Q0–Q42</code></b> — 43 queries, <code>md5_equal</code> against PG 18'
    return details('clickbench', summary, 'Per-query win map of the 43 ClickBench queries: the fastest arm of each tile; amber where a rival beats pg_sift.', body)


# ---- search -------------------------------------------------------------------------------------------------------------------------------

def cold_p50(root):
    rows = [r for r in benchmarks.tsv_rows(benchmarks.results_path(root, 'articles.tsv'))
            if r['tier'] == 'slim-cold' and r['metric'] == 'cold_p50_ms' and r['variant'] == '-' and r['value'] not in ('NA', 'error')]
    got = {k: [float(r['value']) for r in rows if r['engine'] == e] for k, e in COLD_ENGINE.items()}
    return {k: (min(v), max(v)) for k, v in got.items() if v} if rows else COLD_BASELINE


def search(data, root):
    rows = [(SEARCH_LABEL[label], v) for label, _, v in data['search'] if label in SEARCH_LABEL]
    qps = lambda v: f'{v:,}'
    body = [[label] + bold_best([(v[e], qps(v[e])) for e in ('sift', 'es', 'paradedb', 'rum')], max) for label, v in rows]
    table = resultfile.table(['Workload (50K, QPS)', '`pg_sift`', 'Elasticsearch', 'ParadeDB', 'PG `rum`'], body, ['---'] + ['---:'] * 4)
    won = [v for _, v in rows if v['sift'] >= v['es']]
    leads = ', '.join(f'{label} ({qps(v["es"])} vs {qps(v["sift"])})' for label, v in rows if v['sift'] < v['es'])
    ten = data['search_10m']['08']
    cold = cold_p50(root)
    cold_text = ', '.join(f'{COLD_NAME[k]} {lo:.1f}–{hi:.1f} ms' for k, (lo, hi) in cold.items())
    take = [f'`pg_sift` is {span([v["sift"] / v["paradedb"] for _, v in rows])} ParadeDB and ties or beats Elasticsearch on {len(won)} of {len(rows)} workloads; Elasticsearch leads {leads}. '
            '\\*Elasticsearch filters a denormalized field instead of joining.',
            f'10M orders title search `count(*)`: {ms(ten["sift"])} vs Elasticsearch {ms(ten["es"])} and PG GIN {ms(ten["pg"])}. Cold p50: {cold_text}.']
    summary = f'<b>Exact BM25 search</b> — {span([v["sift"] / v["paradedb"] for _, v in rows])} ParadeDB QPS, {len(won)} of {len(rows)} workloads at or above Elasticsearch'
    alt = f'Throughput against index size on 50K Wikipedia articles: pg_sift is {span([v["sift"] / v["paradedb"] for _, v in rows])} ParadeDB; Elasticsearch leads in amber.'
    return details('search', summary, alt, [*paragraphs(take), *table])


# ---- scopes -------------------------------------------------------------------------------------------------------------------------------

def scopes(data, root):
    rows, (sift_rollup, _), suite = scope_stock_tables.rivals(root)
    shown = [r for a in scope_stock_tables.README_ARMS for r in rows if r.arm == a]
    wld, versus = scope_stock_tables.wld, scope_stock_tables.versus
    body = [[f'`{r.arm}`', wld(r.warm), wld(r.cold), versus(r.rollup, sift_rollup)] for r in shown]
    table = resultfile.table(['Rival', 'Warm: win / tie / loss', 'Cold: win / tie / loss', 'Tenant 1 rollup'], body, ['---', '---:', '---:', '---:'])
    s = data['scoped_rollup']
    missing = [a for a in ('ch_join', 'ch_proj') if a not in {r.arm for r in rows}]
    take = [f'Tenant 1 `sum_by_month` on {s["rows"] / 1e6:.1f}M events: `sift_scope` {ms(s["sift_ms"])} vs {s["stock_ms"]:,.0f} ms on stock PG ({x(s["stock_ms"] / s["sift_ms"])}); '
            f'{s["faster_cells"]} of {s["cells"]} warm cells faster than stock PG (`{s["version"]}`). '
            f'Cells read win / tie / loss: `sift_scope` faster / within 0.1 ms or 10% / slower. {scope_stock_tables.where_note(shown)}']
    if missing:
        take.append('ClickHouse arms (' + ', '.join(f'`{a}`' for a in missing) + '): not measured yet.')
    summary = f'<b>Multi-tenant scoped analytics</b> — {s["faster_cells"]} of {s["cells"]} cells faster than stock PostgreSQL, {x(s["stock_ms"] / s["sift_ms"])} on a tenant rollup'
    alt = f'Read latency against write cost of the scope index and its rivals: {s["faster_cells"]} of {s["cells"]} cells faster than stock PostgreSQL ({suite}).'
    return details('scopes', summary, alt, [*paragraphs(take), *table])


# ---- scalability --------------------------------------------------------------------------------------------------------------------------

def scalability(data, root):
    scaling = data['scaling']
    summary = '<b>Scalability and parallelism</b> — 1 to 8 workers, cold I/O by <code>io_method</code>, partitions, concurrent writers'
    alt = 'Small multiples of row scale, parallel workers and cold I/O: speedup of pg_sift from 1 to 8 workers.'
    if not scaling:
        return details('scalability', summary, alt, [not_measured('bench/bench scaling-io --docs', 'v6-scaling.tsv')])
    warm = scaling['warm']
    ladder = sorted({w for v in warm.values() for w in v})
    body = [[probe] + [f'{v[w]:.2f}×' if w in v else DASH for w in ladder] for probe, v in sorted(warm.items())][:6]
    table = resultfile.table(['Warm probe'] + [f'{w} worker' + 's' * (w > 1) for w in ladder], body, ['---'] + ['---:'] * len(ladder))
    best = max(((v[ladder[-1]], p) for p, v in warm.items() if ladder[-1] in v))
    take = [f'Warm scans reach {x(best[0])} at {ladder[-1]} workers ({best[1]}), against 1 worker.']
    if scaling['cold']:
        probe, by = sorted(scaling['cold'].items())[0]
        take.append(f'Cold I/O ({probe}, most workers, read stream on): ' + ', '.join(f'`{m}` {t:,.0f} MB/s' for m, t in sorted(by.items(), key=lambda kv: -kv[1])) + '.')
    return details('scalability', summary, alt, [*paragraphs(take), *table])


# ---- storage ------------------------------------------------------------------------------------------------------------------------------

def storage(data, root):
    size = {label: mib for label, mib, _, _ in data['storage_orders']}
    bm25 = {label: mib for label, mib, _, _ in data['storage_bm25']}
    st, sc, pg = size['pg_sift (sift_table)'], size['pg_sift (sift_col + heap)'], size['PostgreSQL 18 (stock)']
    ch, es = size['ClickHouse 26.9'], size['Elasticsearch 9.5']
    bm_s, bm_p, bm_np, bm_e = bm25['pg_sift (default)'], bm25['ParadeDB (pg_search)'], bm25['pg_sift (positions=off)'], bm25['Elasticsearch 9.5']
    base, now, n = benchmarks.cores(root)
    gate = lambda metric, section, name: _gate_row(base, now, n, metric, section, name)
    body = [
        ['10M orders (`sift_table`)', f'**{st:.0f} MiB**', f'{pg:,.0f} MiB (PG 18)', f'**{x(pg / st)} smaller**', f'vs {ch:.0f} MiB ClickHouse, {es:,.0f} MiB ES'],
        ['10M orders (`sift_col`)', f'**{sc:,.0f} MiB**', f'{pg:,.0f} MiB (PG 18)', f'**−{(1 - sc / pg) * 100:.0f}% vs PG**', 'Shares the PG heap and PK'],
        ['50K Wiki BM25 index', f'**{bm_s:.1f} MiB**', f'{bm_p:.1f} MiB (ParadeDB)', f'**{x(bm_p / bm_s)} smaller**', f'{bm_np:.1f} MiB `positions=off`; {bm_e:.1f} MiB ES'],
        gate('cic_rss_anon_b_per_row', 'S_slopes', 'Scoped build RSS slope'),
        gate('oltp_ins1_wal_b_per_row', 'G_wal', '1-row `INSERT` WAL'),
        gate('delete_2pct_wal_b_per_row', 'G_wal', '2% `DELETE` WAL'),
    ]
    table = resultfile.table(['Dimension', '`pg_sift`', 'Reference', 'Comparison', 'Notes'], body, ['---', '---:', '---:', '---:', '---'])
    take = [f'`sift_table` is {x(pg / st)} smaller than PG 18 and within {abs(st - ch):.0f} MiB of ClickHouse at 10M orders, with the PK and BM25 inside. '
            'Memory is gated flat to 10M+ rows (`≤ 0.5 B/row` slope; caches capped at 16 / 64 MB).',
            'Rows marked `gate` are 6.0 targets until the 6.0 run replaces them. OLTP churn (`10%` `UPDATE`, `5%` `DELETE`, reads during churn): not measured yet.']
    summary = f'<b>Storage, churn and bounded memory</b> — <code>sift_table</code> {x(pg / st)} smaller than PG 18, RSS slope gated at 0.5 B/row'
    alt = f'On-disk size at 10M orders and peak memory by rows: sift_table {x(pg / st)} smaller than PG 18, memory flat to 10M rows.'
    return details('storage', summary, alt, [*paragraphs(take), *table])


def _gate_row(base, now, n, metric, section, name):
    """one gate row: the 6.0 measurement (or its gate), the 5.1.0 baseline and the change"""
    get = (lambda c: benchmarks.slope(c, metric)) if section == 'S_slopes' else (lambda c: c.get(section, metric, n))
    old, new, gate = get(base), get(now), benchmarks.TARGET[metric]
    unit = 'B/row'
    return [name, f'**{new:.1f} {unit}**' if new is not None else f'`gate` {gate}', f'{old:.1f} {unit} (v5.1)',
            f'**−{(1 - new / old) * 100:.1f}%**' if new is not None else '6.0 run pending', 'Scoped `CIC`, `maintenance_work_mem = 4MB`' if section == 'S_slopes' else f'{n:,} rows']


SECTIONS = {'hero': hero, 'analytics': analytics, 'pg-queries': pg_queries, 'clickbench': clickbench, 'search': search, 'scopes': scopes, 'scalability': scalability, 'storage': storage}


def render(root):
    readme = os.path.join(root, 'README.md')
    data = readme_charts.load_live_data(os.path.join(root, 'docs', 'results'))
    for slug, section in SECTIONS.items():
        resultfile.replace_block(readme, f'readme-{slug}', section(data, root))


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--root', default=REPO)
    render(ap.parse_args().root)


if __name__ == '__main__':
    main()
