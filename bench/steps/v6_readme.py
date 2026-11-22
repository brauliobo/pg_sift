#!/usr/bin/env python3
"""Render the compact 5-column README benchmark tables (analytics, BM25 search, storage/scope).

Reads live results from docs/results/ when present (v6-readme.tsv, store-scaling.json,
duckdb-10000000.tsv, articles.tsv) and falls back to the audited committed baseline snapshot
in bench/steps/readme_charts.py so README tables and docs/charts/*.svg always stay in lockstep.
"""
import argparse
import csv
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import resultfile  # noqa: E402
import readme_charts  # noqa: E402

REPO = os.path.dirname(os.path.dirname(HERE))
DEFAULT_TSV = os.path.join(REPO, 'docs', 'results', 'v6-readme.tsv')


def fmt_cell_ms(v):
    if v >= 1000:
        return f'{v:,.0f} ms'
    if v >= 100:
        return f'{v:.1f} ms'
    if v >= 10:
        return f'{v:.1f} ms'
    return f'{v:.2f} ms'


def bold_min(vals):
    best = min(vals)
    return [f'**{fmt_cell_ms(v)}**' if v == best else fmt_cell_ms(v) for v in vals]


def bold_max_qps(vals):
    best = max(vals)
    return [f'**{v:,}**' if v == best else f'{v:,}' for v in vals]


def render_analytics_lines(tsv_path, data):
    # If a dedicated v6-readme.tsv override is present, render it directly.
    if tsv_path and os.path.exists(tsv_path):
        with open(tsv_path, encoding='utf-8') as f:
            rows = list(csv.DictReader((l for l in f if l.strip() and not l.startswith('#')), delimiter='\t'))
        if rows:
            head = ['Workload (10M orders, 3 CPUs)', '`pg_sift`', 'ClickHouse', 'DuckDB', 'PG 18']
            align = ['---', '---:', '---:', '---:', '---:']
            body = [[r['workload'], r['sift_v6'], r.get('clickhouse', '—'), r.get('duckdb', '—'), r['pg18']] for r in rows]
            return resultfile.table(head, body, align)

    head = ['Workload (10M orders, 3 CPUs)', '`pg_sift`', 'ClickHouse', 'DuckDB', 'PG 18']
    align = ['---', '---:', '---:', '---:', '---:']
    short_desc = {
        'Count by country': 'Count by country (`GROUP BY`)',
        'Revenue / month': 'Revenue per month (`sum`)',
        'Top 10 categories': 'Top 10 categories (`LIMIT 10`)',
        'Avg shipped order': 'Avg shipped order (3 filters)',
        'Distinct / month': 'Distinct customers/mo (`DISTINCT`)',
        'Big recent orders': 'Big recent orders (range count)',
    }
    body = []
    for label, _sub, vals in data['analytics']:
        cells = bold_min([vals['sift'], vals['ch'], vals['duckdb'], vals['pg']])
        body.append([short_desc.get(label, label), *cells])
    return resultfile.table(head, body, align)


def render_search_lines(data):
    head = ['Search Workload', '`pg_sift`', 'Elasticsearch', 'ParadeDB', 'PG FTS']
    align = ['---', '---:', '---:', '---:', '---:']
    qps_map = {label: vals for label, _, vals in data['search']}
    qps_rows = [
        ('Exact `count(*)` (50K Wiki, QPS)', 'Exact count(*)'),
        ('Phrase `"a b"` (50K Wiki, QPS)', 'Phrase "a b"'),
        ('Boolean `a AND b` (50K Wiki, QPS)', 'Boolean a AND b'),
        ('Filtered top-k (50K Wiki, QPS)', 'Filtered top-k'),
        ('Boolean `a OR b` (50K Wiki, QPS)', 'Boolean a OR b'),
    ]
    body = []
    for wlabel, key in qps_rows:
        v = qps_map[key]
        s, e, p, r = bold_max_qps([v['sift'], v['es'], v['paradedb'], v['rum']])
        body.append([wlabel, s, e, p, f'{r} *(rum)*'])
    for qid, label in (('08', 'Title search count (10M orders)'), ('10', 'Search + newest 10 (10M orders)')):
        v = data['search_10m'][qid]
        s, e, g = bold_min([v['sift'], v['es'], v['pg']])
        body.append([label, s, e, 'n/a', f'{g} *(GIN)*'])
    return resultfile.table(head, body, align)


def render_storage_lines(data):
    head = ['Dimension', '`pg_sift`', 'Reference', 'Comparison', 'Notes']
    align = ['---', '---:', '---:', '---:', '---']
    ord_sz = {label: mib for label, mib, _, _ in data['storage_orders']}
    bm_sz = {label: mib for label, mib, _, _ in data['storage_bm25']}
    st = ord_sz.get('pg_sift (sift_table)', 456.6)
    sc = ord_sz.get('pg_sift (sift_col + heap)', 1437.5)
    pg = ord_sz.get('PostgreSQL 18 (stock)', 1549.1)
    ch = ord_sz.get('ClickHouse 26.9', 421.4)
    es = ord_sz.get('Elasticsearch 9.5', 1456.9)
    bm_s = bm_sz.get('pg_sift (default)', 30.2)
    bm_np = bm_sz.get('pg_sift (positions=off)', 12.0)
    bm_p = bm_sz.get('ParadeDB (pg_search)', 54.5)
    bm_e = bm_sz.get('Elasticsearch 9.5', 70.5)
    sr = data['scoped_rollup']
    body = [
        ['10M orders (`sift_table`)', f'**{st:.0f} MiB**', f'{pg:,.0f} MiB (PG 18)', f'**{pg / st:.1f}× smaller**', f'vs {ch:.0f} MiB ClickHouse, {es:,.0f} MiB ES'],
        ['10M orders (`sift_col`)', f'**{sc:,.0f} MiB**', f'{pg:,.0f} MiB (PG 18)', f'**−{(1 - sc / pg) * 100:.0f}% vs PG**', 'Shares 1,023 MiB PG heap + PK'],
        ['50K Wiki BM25 index', f'**{bm_s:.1f} MiB**', f'{bm_p:.1f} MiB (ParadeDB)', f'**{bm_p / bm_s:.1f}× smaller**', f'{bm_np:.1f} MiB `positions=off`; {bm_e:.1f} MiB ES'],
        [f'Scoped rollup ({sr["rows"] / 1e6:.1f}M rows)', f'**{fmt_cell_ms(sr["sift_ms"])}**', f'{sr["stock_ms"]:,.0f} ms (PG 18)', f'**{sr["stock_ms"] / sr["sift_ms"]:.0f}× faster**',
         f'{sr["faster_cells"]}/{sr["cells"]} warm wins vs stock PG (`{sr["version"]}`)'],
        ['Bounded memory (v6.0)', '**16 / 64 MB**', 'O(1) flat RSS', '**≤ 0.5 B/row**', '`backend_cache_kb` / `shared_cache_kb`'],
    ]
    return resultfile.table(head, body, align)


def render(results_path, root):
    readme = os.path.join(root, 'README.md')
    data = readme_charts.load_live_data(os.path.join(root, 'docs', 'results'))
    text = open(readme, encoding='utf-8').read()
    resultfile.replace_block(readme, 'v6-readme', render_analytics_lines(results_path, data))
    if '<!-- bench:v6-search:begin' in text:
        resultfile.replace_block(readme, 'v6-search', render_search_lines(data))
    if '<!-- bench:v6-storage:begin' in text:
        resultfile.replace_block(readme, 'v6-storage', render_storage_lines(data))


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--results', default=DEFAULT_TSV)
    ap.add_argument('--root', default=REPO)
    a = ap.parse_args()
    render(a.results, a.root)


if __name__ == '__main__':
    main()
