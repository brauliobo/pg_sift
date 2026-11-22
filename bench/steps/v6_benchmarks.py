#!/usr/bin/env python3
"""Render the generated tables of docs/BENCHMARKS.md from docs/results/ (the `<!-- bench:v6-*:begin/end -->` blocks).

    python3 -I bench/steps/v6_benchmarks.py [--root DIR]

Two kinds of blocks, both functions of the results files only (no figure of a block is typed):
  v6-core.tsv / v6-scaling.tsv (`bench/bench v6-core --docs`, `bench/bench scaling-io --docs`): the 6.0 tables. The 5.1 side is bench/baselines/v510.tsv, so these
  blocks are always rendered; the 6.0 cells say TBD until the file exists.
    v6-scope-build, v6-wide-json, v6-memory, v6-gates, v6-scaling
  store-scaling.json, store.json, articles.tsv, duckdb-10000000.tsv (`bench/bench store|all|duckdb --docs`): the cross-engine tables. A block whose results are not
  in docs/results/ keeps the committed measurement it holds.
    v6-orders-10m, v6-orders-1m, v6-search-warm, v6-search-cold, v6-search-10m, v6-size-orders, v6-size-bm25
"""
import argparse
import json
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import readme_charts  # noqa: E402
import resultfile  # noqa: E402
from readme_charts import QUERY_SLUG, read_tsv  # noqa: E402

REPO = os.path.dirname(os.path.dirname(HERE))
BASELINE = readme_charts.MEMORY_BASELINE
TBD = '*TBD*'
DASH = '—'


def ms(v):
    if v is None:
        return DASH
    return f'{v:,.0f} ms' if v >= 1000 else f'{v:.1f} ms' if v >= 10 else f'{v:.2f} ms'


def bold_best(cells, best):
    """cells = [(value or None, text)]; the text of the best value (min or max) in bold"""
    vals = [v for v, _ in cells if v is not None]
    top = best(vals) if vals else None
    return [f'**{t}**' if v is not None and v == top else t for v, t in cells]


# ---- the cross-engine tables ------------------------------------------------------------------------------------------------------------

def store_rows(path, tier):
    if not os.path.exists(path):
        return None
    with open(path, encoding='utf-8') as f:
        data = json.load(f)
    rows = [r for r in data.get('rows', []) if r.get('tier') == tier]
    return (data.get('meta', {}).get('tiers', {}).get(tier, {}), rows) if rows else None


class Store:
    def __init__(self, got):
        self.meta, self.rows = got
        self.v = {(r['engine'], r['class'], r['variant'], r['metric']): r['value'] for r in self.rows if r['value'] not in ('NA', 'error', None)}

    def get(self, engine, cls='-', variant='-', metric='latency_ms'):
        v = self.v.get((engine, cls, variant, metric))
        return None if v is None else float(v)


def tsv_rows(path):
    return read_tsv(path)[1]


def results_path(root, name):
    return os.path.join(root, 'docs', 'results', name)


def duckdb_warm(root):
    rows = tsv_rows(results_path(root, 'duckdb-10000000.tsv'))
    return {r['case']: float(r['value']) for r in rows if r['arm'] == 'duckdb_parquet' and r['metric'] == 'warm_ms'}


def render_orders_10m(root):
    got = store_rows(results_path(root, 'store-scaling.json'), '10M-c3')
    if not got:
        return None
    S, dk = Store(got), duckdb_warm(root)
    head = ['Query (10M orders)', '`sift_col`', '`sift_table`', 'ClickHouse', 'DuckDB Parquet', 'PG 18']
    names = {'01': 'Point lookup (`WHERE id = 42`)', '02': 'Count by country (`GROUP BY`)', '03': 'Revenue / month (`sum GROUP BY`)', '04': 'Top 10 categories (`LIMIT 10`)',
             '05': 'Avg shipped order (3 filters)', '06': 'Distinct customers / month', '07': 'Big recent orders (range + `>100`)'}
    body = []
    for qid, label in names.items():
        slug = QUERY_SLUG[qid]
        vals = [S.get('sift', slug, 'warm'), S.get('sift_table', slug, 'warm'), S.get('ch', slug, 'warm'), dk.get(slug), S.get('pg', slug, 'warm')]
        body.append([f'`{qid}` {label}'] + bold_best([(v, ms(v)) for v in vals], min))
    return resultfile.table(head, body, ['---'] + ['---:'] * 5)


def render_orders_1m(root):
    got = store_rows(results_path(root, 'store.json'), '1M-c3')
    if not got:
        return None
    S = Store(got)
    head = ['Query (1M orders, warm / cold)', '`sift_col`', '`sift_table`', 'ClickHouse', 'Elasticsearch', 'PG 18']
    engines = ('sift', 'sift_table', 'ch', 'es', 'pg')
    body = []
    for qid in ('02', '03', '04', '05', '06', '07'):
        slug = QUERY_SLUG[qid]
        pairs = [(S.get(e, slug, 'warm'), S.get(e, slug, 'cold')) for e in engines]
        warm = bold_best([(w, ms(w)) for w, _ in pairs], min)
        cold = bold_best([(c, ms(c)) for _, c in pairs], min)
        body.append([f'`{qid}` {readme_charts_label(qid)}'] + [f'{w} / {c}' for w, c in zip(warm, cold)])
    return resultfile.table(head, body, ['---'] + ['---:'] * 5)


def readme_charts_label(qid):
    return next(label for q, label, _ in readme_charts.QUERY_ORDER_ANALYTICS if q == qid)


ENGINES_ARTICLES = (('sift', '`pg_sift` (`sift`)'), ('elasticsearch', 'Elasticsearch 9.5'), ('pg_search', 'ParadeDB (`pg_search`)'), ('rum', 'PG `rum`'), ('pg_textsearch', '`pg_textsearch`'))
CLASSES_ARTICLES = (('count', 'Exact `count(*)`'), ('phrase', 'Phrase `"a b"`'), ('and', 'Boolean `a AND b`'), ('mixed', 'Mixed boolean'),
                    ('filter', 'Filtered top-k'), ('or', 'Boolean `a OR b`'), ('join', 'Joined top-k\\*'))


def articles(root, tier, metric, classes, fmt, best):
    rows = tsv_rows(results_path(root, 'articles.tsv'))
    v = {(r['engine'], r['class']): float(r['value']) for r in rows if r['tier'] == tier and r['metric'] == metric and r['variant'] == '-' and r['value'] not in ('NA', 'error')}
    if not v:
        return None
    return resultfile.table(['Query Class'] + [name for _, name in ENGINES_ARTICLES],
                            [[label] + bold_best([(v.get((e, cls)), fmt(v.get((e, cls)))) for e, _ in ENGINES_ARTICLES], best)
                             for cls, label in classes], ['---'] + ['---:'] * len(ENGINES_ARTICLES))


def render_search_warm(root):
    return articles(root, 'slim', 'qps', CLASSES_ARTICLES, lambda v: DASH if v is None else f'{v:,.0f}', max)


def render_search_cold(root):
    return articles(root, 'slim-cold', 'cold_p50_ms', [c for c in CLASSES_ARTICLES if c[0] != 'mixed'], lambda v: DASH if v is None else f'{v:.2f} ms', min)


def render_search_10m(root):
    got = store_rows(results_path(root, 'store-scaling.json'), '10M-c3')
    if not got:
        return None
    S = Store(got)
    head = ['Query (10M orders, `"wireless headphones"`)', '`sift_col` + `sift`', '`sift_table` + `sift`', 'Elasticsearch 9.5', 'ClickHouse 26.9', 'PG 18 GIN']
    names = {'08': 'Search `count(*)`', '09': 'Search + filter + `GROUP BY category`', '10': 'Search + filter + `ORDER BY ts DESC LIMIT 10`'}
    body = []
    for qid, label in names.items():
        vals = [S.get(e, QUERY_SLUG[qid], 'warm') for e in ('sift', 'sift_table', 'es', 'ch', 'pg')]
        body.append([f'`{qid}` {label}'] + bold_best([(v, ms(v)) for v in vals], min))
    return resultfile.table(head, body, ['---'] + ['---:'] * 5)


NOTES_ORDERS = {'duckdb': 'External file; no PK or BM25', 'ch': 'MergeTree; no PK or BM25', 'sift_table': 'Incl. PK + BM25', 'citus': 'Append-only; no BM25',
                'sift': 'Shares PG heap + PK', 'es': 'Separate JVM cluster', 'pg': 'Heap + PK + B-trees + GIN'}
NAMES_ORDERS = {'duckdb': 'DuckDB (`pg_duckdb` Parquet)', 'ch': 'ClickHouse 26.9', 'sift_table': '**`pg_sift` (`sift_table`)**', 'citus': 'Citus 14.2 columnar',
                'sift': '**`pg_sift` (`sift_col`)**', 'es': 'Elasticsearch 9.5', 'pg': 'PostgreSQL 18 (stock)'}


def render_size_orders(root):
    big = store_rows(results_path(root, 'store-scaling.json'), '10M-c3')
    if not big:
        return None
    S, small = Store(big), store_rows(results_path(root, 'store.json'), '1M-c3')
    one = Store(small) if small else None
    dk = [r for r in tsv_rows(results_path(root, 'duckdb-10000000.tsv')) if r['arm'] == 'duckdb_parquet']
    dkv = {r['metric']: float(r['value']) for r in dk if r['case'] == 'load'}
    size = {e: S.get(e, metric='size_mib') for e in NAMES_ORDERS if e != 'duckdb'}
    load = {e: S.get(e, metric='load_s') for e in NAMES_ORDERS if e != 'duckdb'}
    size['duckdb'], load['duckdb'] = dkv.get('table_mib'), dkv.get('load_s')
    pg = size['pg']
    body = []
    for e in sorted((e for e in size if size[e] is not None), key=lambda e: size[e]):
        notes = NOTES_ORDERS[e] + (f'; **{pg / size[e]:.1f}× smaller** than PG 18' if e == 'sift_table' else '; **%.0f%% smaller** than PG' % ((1 - size[e] / pg) * 100) if e == 'sift' and size[e] < pg else '')
        bold = '**' if e in ('sift', 'sift_table') else ''
        breakdown = DASH
        if one and e != 'duckdb' and one.get(e, metric='size_mib') is not None:
            heap = sum(one.get(e, variant=v, metric='size_mib') or 0 for v in ('table', 'index orders_pkey'))
            total = one.get(e, metric='size_mib')
            breakdown = f'{heap:.1f} / {total - heap:.1f} MiB ({total:.1f} MiB)'
        body.append([NAMES_ORDERS[e], f'{bold}{size[e]:,.1f} MiB{bold}', DASH if load[e] is None else f'{load[e]:.1f} s', breakdown, notes])
    return resultfile.table(['System', '10M Total Size', '10M Load', '1M Breakdown (Heap+PK / Col+Idx)', 'Notes'], body, ['---', '---:', '---:', '---:', '---'])


NAMES_BM25 = (('sift_nopos', '**`pg_sift` (`positions=off`)**', 'No phrase positions'), ('sift', '**`pg_sift` (default)**', 'Full phrase + BM25 + filters'),
              ('pg_textsearch', '`pg_textsearch` 1.5', 'Timescale BM25'), ('pg_search', 'ParadeDB (`pg_search`)', 'Tantivy inside PG'),
              ('elasticsearch', 'Elasticsearch 9.5', 'Separate JVM cluster'), ('rum', 'PG `rum`', 'Requires stored `tsvector` column'))


def render_size_bm25(root):
    rows = [r for r in tsv_rows(results_path(root, 'articles.tsv')) if r['tier'] == 'slim' and r['variant'] == '-' and r['value'] not in ('NA', 'error')]
    v = {(r['engine'], r['metric']): float(r['value']) for r in rows if r['metric'] in ('index_mib', 'build_s', 'heap_growth_mib')}
    if not v:
        return None
    body = []
    for e, name, note in sorted(NAMES_BM25, key=lambda x: v.get((x[0], 'index_mib'), 1e9)):
        if (e, 'index_mib') not in v:
            continue
        grow = v.get((e, 'heap_growth_mib'), 0.0)
        body.append([name, f'{v[(e, "index_mib")]:.1f} MiB', f'{v.get((e, "build_s"), 0.0):.1f} s', f'{grow:+.1f} MiB' if grow else '0.0 MiB', note])
    return resultfile.table(['Engine', 'Index Size', 'Build Time', 'Table Growth', 'Notes'], body, ['---', '---:', '---:', '---:', '---'])


# ---- the 6.0 tables: v6-core.tsv against bench/baselines/v510.tsv ----------------------------------------------------------------------

class Core:
    """the rows of a v6-core.tsv shaped file: section, metric, scale, value, unit, notes"""

    def __init__(self, path):
        self.meta, rows = read_tsv(path)
        self.v = {(r['section'], r['metric'], r['scale']): r for r in rows if r['value'] not in ('', 'n/a')}
        self.scales = sorted({int(r['scale']) for r in rows if r['scale'].isdigit()})
        self.present = bool(rows)

    def get(self, section, metric, scale):
        r = self.v.get((section, metric, str(scale)))
        return None if r is None else float(r['value'])


def cores(root):
    now = Core(results_path(root, 'v6-core.tsv'))
    base = Core(BASELINE)
    scale = max(set(now.scales) & set(base.scales)) if now.present and set(now.scales) & set(base.scales) else max(base.scales)
    return base, now, scale


def cell(v, fmt):
    return TBD if v is None else fmt % v


def pass2_pct(c, n):
    a, b = c.get('A_build_cic', 'pass1_ms', n), c.get('A_build_cic', 'pass2_ms', n)
    return None if not a or b is None else b / a * 100


def temp_bytes_per_row(c, n):
    mb = c.get('A_build_cic', 'temp_mb', n)
    return None if mb is None else mb * 1048576 / n


def render_scope_build(root):
    base, now, n = cores(root)
    rows = [
        ('Plain scoped `CREATE INDEX` wall time', lambda c: c.get('A_build_scoped_plain', 'ratio_vs_entity', n), '%.2f× entity', '≤ 1.15× entity'),
        ('Scoped `CIC` Pass 2 wall time', lambda c: pass2_pct(c, n), '%.0f%% of Pass 1', '≤ 15% of Pass 1'),
        ('Scope build temp-file I/O', lambda c: temp_bytes_per_row(c, n), '%.0f B/row', '**0 B**'),
        ('Scope disk overhead vs entity', lambda c: c.get('G_size', 'scope_overhead_pct', n), '+%.1f%%', '≤ 5% (≤ 1% `cluster`)'),
        ('Scoped whole-key read', lambda c: c.get('D_analytical', 'Q1_scope_whole_1w_ms', n), '%.2f ms', 'O(1) (< 0.5 ms)'),
        ('Scoped month read', lambda c: c.get('D_analytical', 'Q2_scope_month_1w_ms', n), '%.2f ms', 'O(1) (< 0.5 ms)'),
        ('Scoped latest-20 read', lambda c: c.get('D_analytical', 'Q7_scope_top20_unfilt_1w_ms', n), '%.2f ms', 'O(1) (< 0.5 ms)'),
    ]
    body = [[name, cell(get(base), fmt), target, cell(get(now), fmt)] for name, get, fmt, target in rows]
    return [f'Entity-Once scope on the wide + JSONB schema at {n:,} rows (`docs/results/v6-core.tsv`; 5.1 from `bench/baselines/v510.tsv`):', ''] \
        + resultfile.table(['Metric', 'v5.1.0 Measured', '`pg_sift` 6.0 Target', '`pg_sift` 6.0 Measured'], body, ['---', '---:', '---:', '---:'])


def render_wide_json(root):
    base, now, n = cores(root)
    per_row = lambda section, metric: (lambda c: c.get(section, metric, n))
    rows = [
        ('Size, entity index', 'G_size', 'entity_b_per_row', '%.1f B/row', '—', '—', '—'),
        ('Size, scoped index', 'G_size', 'scoped_b_per_row', '%.1f B/row', '—', '—', '≤ 33 B/row (≤ 16 excl. a 16 B UUID)'),
        ('Heap + TOAST', 'G_size', 'heap_b_per_row', '%.1f B/row', '—', '—', '—'),
        ('Page / extent utilization', 'G_size', 'page_utilization_pct', '%.0f%%', '92–96%', '~99%', '≥ 95%'),
        ('Bulk build WAL (`wal_level=replica`)', 'G_wal', 'cic_wal_b_per_row', '%.1f B/row', '—', '—', '≤ 45 B/row'),
        ('1-row OLTP `INSERT` WAL', 'G_wal', 'oltp_ins1_wal_b_per_row', '%.1f B/row', '~460 B/row', 'n/a', '≤ 220 B/row'),
        ('2% `DELETE` WAL (incl. DV fold)', 'G_wal', 'delete_2pct_wal_b_per_row', '%.1f B/row', '~60 B/row', '~40 B/row', '≤ 45 B/row'),
        ('Steady-state compaction WA', 'G_compaction', 'write_amplification', '%.2f×', '—', '3.0–5.0×', '≤ 2.35×'),
    ]
    body = []
    for name, section, metric, fmt, pg, ch, target in rows:
        scale = 40000 if section == 'G_compaction' else n
        body.append([name, pg, ch, cell(base.get(section, metric, scale), fmt), target, cell(now.get(section, metric, scale), fmt)])
    return [f'13-column wide + 3-JSONB schema with `NUMERIC(12,2)`, `UUID`, and a `2.1×` M:N scope at {n:,} rows (`docs/results/v6-core.tsv`; 5.1 from `bench/baselines/v510.tsv`):', ''] \
        + resultfile.table(['Metric', 'PG 18 Heap', 'ClickHouse ZSTD', '`pg_sift` v5.1', '`pg_sift` 6.0 Target', '`pg_sift` 6.0 Measured'], body, ['---'] + ['---:'] * 5)


def render_memory(root):
    base, now, _ = cores(root)
    first, last = '%dk' % (min(base.scales) // 1000), '%dk' % (max(base.scales) // 1000)
    slope = lambda c, metric: next((float(r['value']) for (s, m, sc), r in c.v.items() if s == 'S_slopes' and m == metric), None)
    rows = [
        ('Scoped `CIC` `RssAnon` slope', '`maintenance_work_mem = 4MB`', 'cic_rss_anon_b_per_row'),
        ('Scoped `CIC` temp-file slope', '`maintenance_work_mem = 4MB`', 'cic_temp_b_per_row'),
        ('Scoped index size slope', '—', 'cic_disk_b_per_row'),
        ('Scoped `CIC` WAL slope', '—', 'cic_wal_b_per_row'),
        ('`VACUUM` `RssAnon` slope', '`maintenance_work_mem = 4MB`', 'vac1_rss_anon_b_per_row'),
    ]
    body = [[name, budget, '≤ 0.5 B/row' if 'RssAnon' in name or 'temp' in name else '—', cell(slope(base, m), '%.1f B/row'), cell(slope(now, m), '%.1f B/row')] for name, budget, m in rows]
    idle = lambda c: c.get('E_idle_backend', 'rss_anon_delta_mb', 34)
    body.append(['Idle pooled backends (34 indexes)', '`backend_cache_kb` cap', '≤ cap + 8 MB', cell(idle(base), '+%.1f MB'), cell(idle(now), '+%.1f MB')])
    return [f'Slopes are an OLS fit over the {first}–{last} ladder (`docs/results/v6-core.tsv`; 5.1 from `bench/baselines/v510.tsv`):', ''] \
        + resultfile.table(['Workload', 'Budget', 'Slope gate', 'v5.1.0 Baseline', '`pg_sift` 6.0 Measured'], body, ['---', '---', '---:', '---:', '---:'])


# ---- v6-scaling.tsv ---------------------------------------------------------------------------------------------------------------------

def scaling_rows(root):
    meta, rows = read_tsv(results_path(root, 'v6-scaling.tsv'))
    return meta, rows


def render_gates(root):
    meta, rows = scaling_rows(root)
    gated = [r for r in rows if r['gate_pass'] in ('pass', 'fail', 'notcold')]
    if not gated:
        return ['*Not measured for 6.0 yet: `bench/bench scaling-io --docs` writes `docs/results/v6-scaling.tsv` and populates this block.*']
    body = [[r['suite'], r['probe'], r['io_method'], r['workers'], r['speedup'] and f'{float(r["speedup"]):.2f}×', r['gate_pass']] for r in gated]
    return resultfile.table(['Suite', 'Probe', '`io_method`', 'Workers', 'Speedup', 'Gate'], body, ['---', '---', '---', '---:', '---:', '---'])


def render_scaling(root):
    meta, rows = scaling_rows(root)
    if not rows:
        return ['*Not measured for 6.0 yet: `bench/bench scaling-io --docs` writes `docs/results/v6-scaling.tsv` and populates this block.*']
    warm = [r for r in rows if r['suite'] == 'A' and r['speedup']]
    ladder = sorted({int(r['workers']) for r in warm})
    by = {(r['probe'], int(r['workers'])): r for r in warm}
    out = ['**Warm compute scaling** (speedup of N workers over 1, `docs/results/v6-scaling.tsv`):', '']
    body = [[p] + [f'{float(by[(p, w)]["speedup"]):.2f}×' if (p, w) in by else DASH for w in ladder] for p in sorted({r['probe'] for r in warm})]
    out += resultfile.table(['Probe'] + [f'{w} worker' + 's' * (w > 1) for w in ladder], body, ['---'] + ['---:'] * len(ladder))
    cold = [r for r in rows if r['suite'] == 'B' and r['read_stream'] == 'on' and r['throughput'] and r['gate_pass'] != 'notcold']
    if cold:
        methods = sorted({r['io_method'] for r in cold})
        peak = {}
        for r in cold:
            k = (r['probe'], r['io_method'])
            if k not in peak or int(r['workers']) > int(peak[k]['workers']):
                peak[k] = r
        body = [[p] + [f'{float(peak[(p, m)]["throughput"]):,.0f} MB/s ({peak[(p, m)]["workers"]}w)' if (p, m) in peak else DASH for m in methods] for p in sorted({r['probe'] for r in cold})]
        out += ['', '**Cold I/O throughput** (most workers, read stream on; the cache evicted before every run):', '']
        out += resultfile.table(['Probe'] + [f'`{m}`' for m in methods], body, ['---'] + ['---:'] * len(methods))
    return out


BLOCKS = {
    'v6-orders-10m': render_orders_10m, 'v6-orders-1m': render_orders_1m, 'v6-gates': render_gates, 'v6-search-warm': render_search_warm, 'v6-search-cold': render_search_cold,
    'v6-search-10m': render_search_10m, 'v6-scope-build': render_scope_build, 'v6-size-orders': render_size_orders, 'v6-size-bm25': render_size_bm25,
    'v6-wide-json': render_wide_json, 'v6-memory': render_memory, 'v6-scaling': render_scaling,
}


def render(root):
    page = os.path.join(root, 'docs', 'BENCHMARKS.md')
    text = open(page, encoding='utf-8').read()
    for name, renderer in BLOCKS.items():
        if '<!-- bench:%s:begin' % name in text:
            lines = renderer(root)
            if lines is not None:
                resultfile.replace_block(page, name, lines)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--root', default=REPO)
    render(ap.parse_args().root)


if __name__ == '__main__':
    main()
