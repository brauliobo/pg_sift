#!/usr/bin/env python3
"""Generate publication-quality SVG benchmark charts in docs/charts/ (standard library only).

Reads live results from docs/results/ when present (store-scaling.json, duckdb-10000000.tsv,
articles.tsv, store.json) and falls back to the audited committed baseline snapshot when raw
result files are absent (such as in the public repository checkout).

Every chart uses adaptive CSS (@media (prefers-color-scheme: dark)) with its own card surface
and high-contrast text so it renders crisply on both GitHub Light and GitHub Dark themes.
Output is deterministic: identical inputs produce identical bytes.

Charts written:
  docs/charts/analytics-engines.svg   10M orders warm latency: pg_sift vs ClickHouse, DuckDB, PG 18
  docs/charts/search-bm25.svg         50K Wikipedia warm QPS: pg_sift vs Elasticsearch, ParadeDB, PG rum
  docs/charts/storage-footprint.svg   10M orders total disk size & 50K Wikipedia BM25 index size
  docs/charts/scaling-parallel-io.svg warm speedup of 1 to 8 workers against linear, and cold I/O throughput by io_method (docs/results/v6-scaling.tsv)
  docs/charts/memory-flatness.svg     peak RssAnon of the scoped build over the scale ladder, 5.1 against 6.0, and index and WAL bytes a row (docs/results/v6-core.tsv)

The two charts of 6.0 draw the 5.1 baseline from bench/baselines/v510.tsv; without docs/results/v6-core.tsv or v6-scaling.tsv their 6.0 side says it is not measured yet.
"""
import argparse
import csv
import json
import math
import os
import sys
from xml.sax.saxutils import escape

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))

# Audited committed baseline measurements from docs/results/ (main branch, 2026-10-27):
#   - store-scaling.json (tier 10M-c3, 10,000,000 orders, 3 CPUs, 8 GiB RAM, AMD Ryzen 7 9700X)
#   - duckdb-10000000.tsv (10,000,000 orders, 3 CPUs, 8 GiB RAM, pg_duckdb 1.1.1 / DuckDB 1.4.3)
#   - articles.tsv (tier slim, 50,000 Simple English Wikipedia articles, 8 clients, 2026-10-27)
# Running bench/ suites writes updated files to docs/results/ and regenerates these charts automatically.
BASELINE = {
    'analytics_meta': {
        'subtitle': '10,000,000 orders · 3 CPUs, 8 GiB RAM · warm median latency (ms, log scale, lower is better)',
        'footnote': 'Data: store-scaling.json (10M-c3) & duckdb-10000000.tsv · Ryzen 7 9700X · 2026-10-27 (pg_sift v3.5, PG 18.6, CH 26.9, DuckDB 1.4.3).',
    },
    'analytics': [
        # (label, sublabel, {engine: warm_ms})
        ('Count by country', 'count(*) GROUP BY country', {'sift': 0.85, 'ch': 9.32, 'duckdb': 8.97, 'pg': 188.90}),
        ('Revenue / month', 'sum(amount) GROUP BY month', {'sift': 0.93, 'ch': 23.79, 'duckdb': 99.17, 'pg': 1377.23}),
        ('Top 10 categories', 'sum(amount) GROUP BY LIMIT 10', {'sift': 0.90, 'ch': 20.96, 'duckdb': 26.67, 'pg': 370.20}),
        ('Avg shipped order', 'avg(amount) WHERE 3 filters', {'sift': 10.04, 'ch': 7.68, 'duckdb': 48.89, 'pg': 124.24}),
        ('Distinct / month', 'count(DISTINCT customer_id)', {'sift': 49.34, 'ch': 104.54, 'duckdb': 193.70, 'pg': 2333.82}),
        ('Big recent orders', 'count(*) WHERE range + amount', {'sift': 10.63, 'ch': 4.39, 'duckdb': 39.73, 'pg': 146.44}),
    ],
    'search_meta': {
        'subtitle': '50,000 Simple English Wikipedia articles · 8 clients · warm throughput (queries/sec, higher is better)',
        'footnote': 'Data: articles.tsv (slim) · Ryzen 7 9700X · 2026-10-27 (pg_sift v3.5, ES 9.5.5, ParadeDB 0.26.1, PG rum 1.3). *ES join uses denorm filter.',
    },
    'search': [
        # (label, sublabel, {engine: qps})
        ('Exact count(*)', 'matching document count', {'sift': 29127, 'es': 28105, 'paradedb': 2399, 'rum': 229}),
        ('Phrase "a b"', 'exact proximity BM25 top-10', {'sift': 21794, 'es': 19423, 'paradedb': 2332, 'rum': 184}),
        ('Boolean a AND b', '2-term conjunction BM25', {'sift': 21690, 'es': 17476, 'paradedb': 2414, 'rum': 4909}),
        ('Mixed boolean', 'AND / OR / NOT BM25 top-10', {'sift': 14159, 'es': 14749, 'paradedb': 2248, 'rum': 112}),
        ('Filtered top-k', 'BM25 + INCLUDE filter pushdown', {'sift': 12604, 'es': 13854, 'paradedb': 2241, 'rum': 113}),
        ('Boolean a OR b', '2-term disjunction BM25', {'sift': 8201, 'es': 11021, 'paradedb': 2000, 'rum': 50}),
        ('Joined top-k*', 'BM25 joined with dimension table', {'sift': 6689, 'es': 14037, 'paradedb': 431, 'rum': 110}),
    ],
    'storage_meta': {
        'subtitle': 'On-disk size (MiB, lower is better) · 10M orders total footprint (top) and 50K Wikipedia BM25 index (bottom)',
        'footnote': 'Data: store-scaling.json (10M-c3), duckdb-10000000.tsv & articles.tsv · 2026-10-27 (v3.5 baseline). PG rum adds 100.3 MiB stored tsvector.',
    },
    'storage_orders': [
        # (label, mib, color_key, note)
        ('DuckDB (Parquet)', 297.3, 'duckdb', 'external file · no PK/BM25'),
        ('ClickHouse 26.9', 421.4, 'ch', 'MergeTree · no PK/BM25'),
        ('pg_sift (sift_table)', 456.6, 'sift_table', 'col + PK + BM25 (3.4× vs PG)'),
        ('Citus 14.2 columnar', 463.4, 'pg', 'col + PK · no BM25'),
        ('pg_sift (sift_col + heap)', 1437.5, 'sift', '1,023 MiB heap + 415 idx'),
        ('Elasticsearch 9.5', 1456.9, 'es', 'Lucene index + _source'),
        ('PostgreSQL 18 (stock)', 1549.1, 'pg', '1,023 MiB heap + 526 idx'),
    ],
    'storage_bm25': [
        # (label, mib, color_key, note)
        ('pg_sift (positions=off)', 12.0, 'sift_table', '0.6 s build'),
        ('pg_sift (default)', 30.2, 'sift', '0.7 s · full positions'),
        ('pg_textsearch 1.5', 40.1, 'duckdb', '5.1 s build'),
        ('ParadeDB (pg_search)', 54.5, 'paradedb', '0.6 s build'),
        ('Elasticsearch 9.5', 70.5, 'es', '3.3 s + 1.3 s merge'),
        ('PG rum (tsvector)', 85.7, 'pg', '3.8 s (+100 MiB table)'),
    ],
    'search_10m': {   # store-scaling.json 10M-c3 warm median ms: {class: {engine: ms}}
        '08': {'sift': 2.35, 'es': 11.57, 'pg': 78.53},
        '10': {'sift': 13.13, 'es': 13.63, 'pg': 141.68},
    },
    'scoped_rollup': {'rows': 2400007, 'sift_ms': 2.18, 'stock_ms': 542.0, 'faster_cells': 72, 'cells': 72, 'version': 'v5.0'},   # scope-stock.tsv, tenant 1 sum_by_month
}

QUERY_ORDER_ANALYTICS = [
    ('02', 'Count by country', 'count(*) GROUP BY country'),
    ('03', 'Revenue / month', 'sum(amount) GROUP BY month'),
    ('04', 'Top 10 categories', 'sum(amount) GROUP BY LIMIT 10'),
    ('05', 'Avg shipped order', 'avg(amount) WHERE 3 filters'),
    ('06', 'Distinct / month', 'count(DISTINCT customer_id)'),
    ('07', 'Big recent orders', 'count(*) WHERE range + amount'),
]

QUERY_SLUG = {   # the class of a store results row (bench/steps/ids: every query has a descriptive slug) of the numbered queries of the docs
    '01': 'point-lookup', '02': 'orders-per-country', '03': 'revenue-per-month', '04': 'top-categories', '05': 'average-shipped-order', '06': 'customers-per-month',
    '07': 'big-recent-orders', '08': 'search-count', '09': 'search-facets', '10': 'search-newest',
}

QUERY_ORDER_SEARCH = [
    ('count', 'Exact count(*)', 'matching document count'),
    ('phrase', 'Phrase "a b"', 'exact proximity BM25 top-10'),
    ('and', 'Boolean a AND b', '2-term conjunction BM25'),
    ('mixed', 'Mixed boolean', 'AND / OR / NOT BM25 top-10'),
    ('filter', 'Filtered top-k', 'BM25 + INCLUDE filter pushdown'),
    ('or', 'Boolean a OR b', '2-term disjunction BM25'),
    ('join', 'Joined top-k*', 'BM25 joined with dimension table'),
]

WIDTH = 860
FONT = 'Inter, -apple-system, BlinkMacSystemFont, "Segoe UI", Helvetica, Arial, sans-serif'

# Adaptive light/dark CSS embedded directly in each SVG so contrast is >10:1 on both themes.
SVG_STYLE = """
  <style>
    .card { fill: #ffffff; stroke: #d0d7de; }
    .title { fill: #1f2328; font-weight: 700; }
    .sub { fill: #57606a; }
    .label { fill: #1f2328; font-weight: 600; }
    .sublabel { fill: #57606a; }
    .tick { fill: #57606a; }
    .val { fill: #1f2328; font-weight: 600; }
    .val-muted { fill: #57606a; }
    .grid { stroke: #e5e7eb; }
    .axis { stroke: #8c959f; }
    .sep { stroke: #e5e7eb; }
    .sec { fill: #1f2328; font-weight: 700; }
    .c-sift { fill: #2563eb; }
    .c-sift_table { fill: #059669; }
    .c-ch { fill: #d97706; }
    .c-duckdb { fill: #0d9488; }
    .c-es { fill: #7c3aed; }
    .c-paradedb { fill: #ea580c; }
    .c-pg { fill: #64748b; }
    .c-rum { fill: #64748b; }
    @media (prefers-color-scheme: dark) {
      .card { fill: #0d1117; stroke: #30363d; }
      .title { fill: #e6edf3; }
      .sub { fill: #8b949e; }
      .label { fill: #e6edf3; }
      .sublabel { fill: #8b949e; }
      .tick { fill: #8b949e; }
      .val { fill: #e6edf3; }
      .val-muted { fill: #8b949e; }
      .grid { stroke: #21262d; }
      .axis { stroke: #484f58; }
      .sep { stroke: #21262d; }
      .sec { fill: #e6edf3; }
      .c-sift { fill: #3b82f6; }
      .c-sift_table { fill: #10b981; }
      .c-ch { fill: #f59e0b; }
      .c-duckdb { fill: #14b8a6; }
      .c-es { fill: #a78bfa; }
      .c-paradedb { fill: #fb923c; }
      .c-pg { fill: #94a3b8; }
      .c-rum { fill: #94a3b8; }
    }
  </style>"""


def fnum(x):
    s = f'{x:.1f}'
    return s[:-2] if s.endswith('.0') else s


def fmt_ms(v):
    if v >= 1000:
        return f'{v:,.0f} ms'
    if v >= 100:
        return f'{v:.0f} ms'
    if v >= 10:
        return f'{v:.1f} ms'
    return f'{v:.2f} ms'


def fmt_qps(v):
    if v >= 10000:
        return f'{v / 1000:.1f}k'
    if v >= 1000:
        return f'{v:,.0f}'
    return f'{v:.0f}'


def fmt_mib(v):
    return f'{v:,.0f} MiB' if v >= 100 else f'{v:.1f} MiB'


def load_live_data(results_dir):
    """Overlay real results from docs/results/ onto BASELINE when present."""
    data = {
        'analytics_meta': dict(BASELINE['analytics_meta']),
        'analytics': [(l, s, dict(v)) for l, s, v in BASELINE['analytics']],
        'search_meta': dict(BASELINE['search_meta']),
        'search': [(l, s, dict(v)) for l, s, v in BASELINE['search']],
        'storage_meta': dict(BASELINE['storage_meta']),
        'storage_orders': list(BASELINE['storage_orders']),
        'storage_bm25': list(BASELINE['storage_bm25']),
        'search_10m': {q: dict(v) for q, v in BASELINE['search_10m'].items()},
        'scoped_rollup': dict(BASELINE['scoped_rollup']),
        'scaling': load_scaling(os.path.join(results_dir or '', 'v6-scaling.tsv')),
        'memory': load_memory(os.path.join(results_dir or '', 'v6-core.tsv')),
    }
    if not results_dir or not os.path.isdir(results_dir):
        return data

    # 1. Analytical 10M store-scaling.json + duckdb-10000000.tsv
    sc_path = os.path.join(results_dir, 'store-scaling.json')
    dk_path = os.path.join(results_dir, 'duckdb-10000000.tsv')
    if os.path.exists(sc_path):
        sc = json.load(open(sc_path, encoding='utf-8'))
        trows = [r for r in sc.get('rows', []) if r.get('tier') == '10M-c3']
        if trows:
            dk_map = {}
            dk_sz = None
            if os.path.exists(dk_path):
                with open(dk_path, encoding='utf-8') as f:
                    lines = [l.rstrip('\n') for l in f if l.strip() and not l.startswith('#')]
                for r in csv.DictReader(lines, delimiter='\t'):
                    if r.get('arm') == 'duckdb_parquet' and r.get('metric') == 'warm_ms':
                        dk_map[r['case']] = float(r['value'])
                    elif r.get('arm') == 'duckdb_parquet' and r.get('metric') == 'table_mib':
                        dk_sz = float(r['value'])
            updated = []
            for qid, label, sub in QUERY_ORDER_ANALYTICS:
                vals = {}
                for eng in ('sift', 'ch', 'pg'):
                    hit = [r['value'] for r in trows if r['engine'] == eng and r['class'] == QUERY_SLUG[qid] and r['variant'] == 'warm' and r['metric'] == 'latency_ms']
                    if hit:
                        vals[eng] = round(float(hit[0]), 2)
                if QUERY_SLUG[qid] in dk_map:
                    vals['duckdb'] = round(dk_map[QUERY_SLUG[qid]], 2)
                if len(vals) == 4:
                    updated.append((label, sub, vals))
            if len(updated) == len(QUERY_ORDER_ANALYTICS):
                data['analytics'] = updated
                tier_meta = sc.get('meta', {}).get('tiers', {}).get('10M-c3', {})
                sv = tier_meta.get('servers', {})
                dt = tier_meta.get('parts', {}).get('warm', {}).get('date', '2026-10-27')[:10]
                cpu = sc.get('meta', {}).get('host', {}).get('cpu', 'AMD Ryzen 7 9700X').split(' 8-Core')[0]
                sift_ver = sv.get('sift_sift', '3.5')
                pg_ver = sv.get('pg', '18.6').split()[0]
                ch_ver = sv.get('ch', '26.9').rsplit('.', 2)[0]
                data['analytics_meta']['footnote'] = (
                    f'Data: store-scaling.json (10M-c3) & duckdb-10000000.tsv · {cpu} · {dt} '
                    f'(pg_sift v{sift_ver}, PG {pg_ver}, CH {ch_ver}, DuckDB 1.4.3).'
                )
            latency = lambda eng, qid: next((float(r['value']) for r in trows if r['engine'] == eng and r['class'] == QUERY_SLUG[qid] and r['variant'] == 'warm' and r['metric'] == 'latency_ms'), None)
            for qid, got in data['search_10m'].items():
                live = {eng: latency(eng, qid) for eng in got}
                if None not in live.values():
                    got.update(live)
            # Update 10M order sizes
            sz_of_variant = lambda eng, variant: next(float(r['value']) for r in trows if r['engine'] == eng and r['metric'] == 'size_mib' and r['variant'] == variant)
            sz_of = lambda eng: next((float(r['value']) for r in trows if r['engine'] == eng and r['metric'] == 'size_mib' and r['variant'] == '-'), None)
            if all(sz_of(e) is not None for e in ('ch', 'sift_table', 'citus', 'sift', 'es', 'pg')):
                pg_sz = sz_of('pg')
                st_sz = sz_of('sift_table')
                ratio = pg_sz / st_sz
                heap_of = lambda eng: sz_of_variant(eng, 'table')
                idx_note = lambda eng: f'{heap_of(eng):,.0f} MiB heap + {sz_of(eng) - heap_of(eng):,.0f} idx'
                items = [
                    ('ClickHouse 26.9', sz_of('ch'), 'ch', 'MergeTree · no PK/BM25'),
                    ('pg_sift (sift_table)', st_sz, 'sift_table', f'col + PK + BM25 ({ratio:.1f}× vs PG)'),
                    ('Citus 14.2 columnar', sz_of('citus'), 'pg', 'col + PK · no BM25'),
                    ('pg_sift (sift_col + heap)', sz_of('sift'), 'sift', idx_note('sift')),
                    ('Elasticsearch 9.5', sz_of('es'), 'es', 'Lucene index + _source'),
                    ('PostgreSQL 18 (stock)', pg_sz, 'pg', idx_note('pg')),
                ]
                if dk_sz is not None:
                    items.insert(0, ('DuckDB (Parquet)', dk_sz, 'duckdb', 'external file · no PK/BM25'))
                data['storage_orders'] = sorted(items, key=lambda x: x[1])

    scoped = load_scoped_rollup(os.path.join(results_dir, 'scope-stock.tsv'))
    if scoped:
        data['scoped_rollup'] = scoped

    # 2. Search articles.tsv (slim tier)
    art_path = os.path.join(results_dir, 'articles.tsv')
    if os.path.exists(art_path):
        with open(art_path, encoding='utf-8') as f:
            lines = [l.rstrip('\n') for l in f if l.strip() and not l.startswith('#')]
        rows = [r for r in csv.DictReader(lines, delimiter='\t') if r.get('tier') == 'slim' and r.get('variant') == '-']
        if rows:
            updated = []
            emap = {'sift': 'sift', 'es': 'elasticsearch', 'paradedb': 'pg_search', 'rum': 'rum'}
            for cls, label, sub in QUERY_ORDER_SEARCH:
                vals = {}
                for k, eng in emap.items():
                    hit = [r['value'] for r in rows if r['engine'] == eng and r['class'] == cls and r['metric'] == 'qps']
                    if hit and hit[0] not in ('NA', 'error'):
                        vals[k] = round(float(hit[0]))
                if len(vals) == 4:
                    updated.append((label, sub, vals))
            if len(updated) == len(QUERY_ORDER_SEARCH):
                data['search'] = updated
            sz_map = {r['engine']: float(r['value']) for r in rows if r['metric'] == 'index_mib' and r['value'] not in ('NA', 'error')}
            bd_map = {r['engine']: float(r['value']) for r in rows if r['metric'] == 'build_s' and r['value'] not in ('NA', 'error')}
            if all(e in sz_map for e in ('sift_nopos', 'sift', 'pg_textsearch', 'pg_search', 'elasticsearch', 'rum')):
                data['storage_bm25'] = [
                    ('pg_sift (positions=off)', sz_map['sift_nopos'], 'sift_table', f'{bd_map.get("sift_nopos", 0.6):.1f} s build'),
                    ('pg_sift (default)', sz_map['sift'], 'sift', f'{bd_map.get("sift", 0.7):.1f} s · full positions'),
                    ('pg_textsearch 1.5', sz_map['pg_textsearch'], 'duckdb', f'{bd_map.get("pg_textsearch", 5.1):.1f} s build'),
                    ('ParadeDB (pg_search)', sz_map['pg_search'], 'paradedb', f'{bd_map.get("pg_search", 0.6):.1f} s build'),
                    ('Elasticsearch 9.5', sz_map['elasticsearch'], 'es', f'{bd_map.get("elasticsearch", 3.3):.1f} s + 1.3 s merge'),
                    ('PG rum (tsvector)', sz_map['rum'], 'pg', f'{bd_map.get("rum", 3.8):.1f} s (+100 MiB table)'),
                ]
    return data


def load_scoped_rollup(path):
    """the scoped rollup row of docs/results/scope-stock.tsv (tenant 1 `sum_by_month`, scope index against the plain statement), None without the file or the cells"""
    if not os.path.exists(path):
        return None
    import scope_stock_tables
    R = scope_stock_tables.Results(path)
    sift, stock = (R.get(1, 'sum_by_month', arm, 'warm_ms') for arm in (scope_stock_tables.BASE, 'stock_default'))
    if sift is None or stock is None:
        return None
    cells, faster, _, _, _ = scope_stock_tables.tally_row(R, 'stock_default', 'warm_ms')
    return {'rows': int(R.meta['facts']), 'sift_ms': sift, 'stock_ms': stock, 'faster_cells': faster, 'cells': cells, 'version': 'v%s' % R.meta['pg_sift']}


def read_tsv(path):
    """(meta, rows) of a tab separated file with `# key=value` meta lines and a header row; ({}, []) when the file is not there"""
    if not path or not os.path.exists(path):
        return {}, []
    with open(path, encoding='utf-8') as f:
        lines = [l.rstrip('\n') for l in f if l.strip()]
    meta = dict(l[2:].split('=', 1) for l in lines if l.startswith('# ') and '=' in l)
    return meta, list(csv.DictReader([l for l in lines if not l.startswith('#')], delimiter='\t'))


def load_scaling(path):
    """{'meta', 'warm': {probe: {workers: speedup}}, 'cold': {probe: {io_method: MB/s}}} of docs/results/v6-scaling.tsv (the cold rows: the stream on, the most workers), None without it"""
    meta, rows = read_tsv(path)
    if not rows:
        return None
    warm, cold = {}, {}
    for r in rows:
        if r['suite'] == 'A' and r['speedup']:
            warm.setdefault(r['probe'], {})[int(r['workers'])] = float(r['speedup'])
        elif r['suite'] == 'B' and r['read_stream'] == 'on' and r['throughput'] and r['gate_pass'] != 'notcold':
            at = cold.setdefault(r['probe'], {})
            if r['io_method'] not in at or int(r['workers']) > at[r['io_method']][0]:
                at[r['io_method']] = (int(r['workers']), float(r['throughput']))
    return {'meta': meta, 'warm': warm, 'cold': {p: {m: v[1] for m, v in at.items()} for p, at in cold.items()}}


MEMORY_BASELINE = os.path.join(os.path.dirname(HERE), 'baselines', 'v510.tsv')


def load_memory(path):
    """{'v51': ladder, '6.0': ladder or None, 'meta'} with ladder = {rows: {'rss': MB, 'size': B/row, 'wal': B/row}} of the scoped CIC build (section A_build_cic)"""
    def ladder(file):
        meta, rows = read_tsv(file)
        got = {}
        for r in rows:
            if r['section'] == 'A_build_cic' and r['scale'].isdigit():
                n = int(r['scale'])
                v = float(r['value'])
                key = {'peak_rss_anon_mb': 'rss', 'size_mb': 'size', 'wal_mb': 'wal'}.get(r['metric'])
                if key:
                    got.setdefault(n, {})[key] = v if key == 'rss' else v * 1048576 / n
        return meta, {n: v for n, v in got.items() if len(v) == 3}
    _, base = ladder(MEMORY_BASELINE)
    meta, now = ladder(path)
    return {'v51': base, 'v6': now or None, 'meta': meta}


def svg_wrap(height, title, desc, body, extra_style=''):
    return (
        f'<svg xmlns="http://www.w3.org/2000/svg" width="{WIDTH}" height="{height}" '
        f'viewBox="0 0 {WIDTH} {height}" role="img" aria-labelledby="t d" '
        f'font-family=\'{FONT}\'>\n'
        f'  <title id="t">{escape(title)}</title>\n'
        f'  <desc id="d">{escape(desc)}</desc>'
        f'{SVG_STYLE}{extra_style}\n'
        f'  <rect class="card" x="0.5" y="0.5" width="{WIDTH - 1}" height="{height - 1}" rx="10" stroke-width="1"/>\n'
        + '\n'.join(body)
        + '\n</svg>\n'
    )


def draw_legend(body, x, y, items):
    cx = x
    for cls, label in items:
        body.append(f'  <rect class="c-{cls}" x="{fnum(cx)}" y="{fnum(y - 9)}" width="12" height="10" rx="2.5"/>')
        body.append(f'  <text class="label" x="{fnum(cx + 17)}" y="{fnum(y)}" font-size="11.5">{escape(label)}</text>')
        cx += 17 + len(label) * 6.6 + 18


def render_analytics_svg(data):
    title = 'Analytical Aggregations: pg_sift vs ClickHouse, DuckDB, and PostgreSQL 18'
    sub = data['analytics_meta']['subtitle']
    foot = data['analytics_meta']['footnote']
    rows = data['analytics']
    series = [
        ('sift', 'pg_sift (sift_col)'),
        ('ch', 'ClickHouse 26.9'),
        ('duckdb', 'DuckDB (pg_duckdb Parquet)'),
        ('pg', 'PostgreSQL 18'),
    ]

    left = 186
    right = 76
    pw = WIDTH - left - right
    top = 104
    bar_h = 12
    bar_gap = 3
    group_h = len(series) * (bar_h + bar_gap) + 18
    plot_h = len(rows) * group_h - 8

    # Log scale: 0.2 ms to 5,000 ms (covers 0.85 ms .. 2,334 ms with comfortable label margin)
    lo, hi = 0.2, 5000.0
    log_lo, log_hi = math.log10(lo), math.log10(hi)
    X = lambda v: left + (math.log10(max(v, lo)) - log_lo) / (log_hi - log_lo) * pw

    body = [
        f'  <text class="title" x="24" y="34" font-size="16">{escape(title)}</text>',
        f'  <text class="sub" x="24" y="53" font-size="11.5">{escape(sub)}</text>',
    ]
    draw_legend(body, 24, 78, series)

    # Vertical gridlines at decades
    ticks = [0.5, 1, 5, 10, 50, 100, 500, 1000, 5000]
    for t in ticks:
        tx = X(t)
        is_dec = t in (1, 10, 100, 1000)
        dash = '' if is_dec else 'stroke-dasharray="3 3"'
        body.append(f'  <line class="grid" x1="{fnum(tx)}" y1="{top - 6}" x2="{fnum(tx)}" y2="{top + plot_h}" stroke-width="{"1.1" if is_dec else "0.7"}" {dash}/>')
        lbl = f'{t:g} ms' if t < 1000 else f'{t // 1000} s'
        body.append(f'  <text class="tick" x="{fnum(tx)}" y="{top + plot_h + 16}" font-size="10.5" text-anchor="middle">{lbl}</text>')
    body.append(f'  <line class="axis" x1="{left}" y1="{top - 6}" x2="{left}" y2="{top + plot_h}" stroke-width="1"/>')

    for gi, (label, sublbl, vals) in enumerate(rows):
        gy = top + gi * group_h
        if gi > 0:
            body.append(f'  <line class="sep" x1="24" y1="{fnum(gy - 9)}" x2="{WIDTH - 24}" y2="{fnum(gy - 9)}" stroke-width="0.7" stroke-opacity="0.7"/>')
        mid_y = gy + (len(series) * (bar_h + bar_gap) - bar_gap) / 2
        body.append(f'  <text class="label" x="{left - 12}" y="{fnum(mid_y - 3)}" font-size="12" text-anchor="end">{escape(label)}</text>')
        body.append(f'  <text class="sublabel" x="{left - 12}" y="{fnum(mid_y + 12)}" font-size="10" text-anchor="end">{escape(sublbl)}</text>')
        best = min(vals.values())
        for si, (eng, _) in enumerate(series):
            v = vals[eng]
            by = gy + si * (bar_h + bar_gap)
            bw = max(4.0, X(v) - left)
            body.append(f'  <rect class="c-{eng}" x="{left}" y="{fnum(by)}" width="{fnum(bw)}" height="{bar_h}" rx="2.5"/>')
            vcls = 'val' if v == best else 'val-muted'
            body.append(f'  <text class="{vcls}" x="{fnum(left + bw + 6)}" y="{fnum(by + 10)}" font-size="10.5">{escape(fmt_ms(v))}</text>')

    foot_y = top + plot_h + 38
    body.append(f'  <text class="sub" x="24" y="{foot_y}" font-size="10">{escape(foot)}</text>')
    height = foot_y + 18
    desc = '; '.join(f'{l}: pg_sift {fmt_ms(v["sift"])}, ClickHouse {fmt_ms(v["ch"])}, DuckDB {fmt_ms(v["duckdb"])}, PG 18 {fmt_ms(v["pg"])}' for l, _, v in rows)
    return svg_wrap(height, title, desc, body)


def render_search_svg(data):
    title = 'Exact BM25 Full-Text Search: pg_sift vs Elasticsearch, ParadeDB, and PG rum'
    sub = data['search_meta']['subtitle']
    foot = data['search_meta']['footnote']
    rows = data['search']
    series = [
        ('sift', 'pg_sift (sift)'),
        ('es', 'Elasticsearch 9.5'),
        ('paradedb', 'ParadeDB (pg_search)'),
        ('rum', 'PG rum (tsvector)'),
    ]

    left = 186
    right = 68
    pw = WIDTH - left - right
    top = 104
    bar_h = 11
    bar_gap = 2.5
    group_h = len(series) * (bar_h + bar_gap) + 17
    plot_h = len(rows) * group_h - 8

    vmax = max(max(v.values()) for _, _, v in rows)
    hi = math.ceil(vmax / 5000.0) * 5000.0
    X = lambda v: left + (v / hi) * pw

    body = [
        f'  <text class="title" x="24" y="34" font-size="16">{escape(title)}</text>',
        f'  <text class="sub" x="24" y="53" font-size="11.5">{escape(sub)}</text>',
    ]
    draw_legend(body, 24, 78, series)

    step = 5000
    t = 0
    while t <= hi + 1:
        tx = X(t)
        body.append(f'  <line class="grid" x1="{fnum(tx)}" y1="{top - 6}" x2="{fnum(tx)}" y2="{top + plot_h}" stroke-width="0.9"/>')
        lbl = '0' if t == 0 else f'{int(t // 1000)}k'
        body.append(f'  <text class="tick" x="{fnum(tx)}" y="{top + plot_h + 16}" font-size="10.5" text-anchor="middle">{lbl}</text>')
        t += step
    body.append(f'  <line class="axis" x1="{left}" y1="{top - 6}" x2="{left}" y2="{top + plot_h}" stroke-width="1"/>')

    for gi, (label, sublbl, vals) in enumerate(rows):
        gy = top + gi * group_h
        if gi > 0:
            body.append(f'  <line class="sep" x1="24" y1="{fnum(gy - 8)}" x2="{WIDTH - 24}" y2="{fnum(gy - 8)}" stroke-width="0.7" stroke-opacity="0.7"/>')
        mid_y = gy + (len(series) * (bar_h + bar_gap) - bar_gap) / 2
        body.append(f'  <text class="label" x="{left - 12}" y="{fnum(mid_y - 3)}" font-size="12" text-anchor="end">{escape(label)}</text>')
        body.append(f'  <text class="sublabel" x="{left - 12}" y="{fnum(mid_y + 11)}" font-size="10" text-anchor="end">{escape(sublbl)}</text>')
        best = max(vals.values())
        for si, (eng, _) in enumerate(series):
            v = vals[eng]
            by = gy + si * (bar_h + bar_gap)
            bw = max(3.5, X(v) - left)
            body.append(f'  <rect class="c-{eng}" x="{left}" y="{fnum(by)}" width="{fnum(bw)}" height="{bar_h}" rx="2.5"/>')
            vcls = 'val' if v == best else 'val-muted'
            body.append(f'  <text class="{vcls}" x="{fnum(left + bw + 6)}" y="{fnum(by + 9.5)}" font-size="10.5">{escape(fmt_qps(v))}</text>')

    foot_y = top + plot_h + 38
    body.append(f'  <text class="sub" x="24" y="{foot_y}" font-size="9.8">{escape(foot)}</text>')
    height = foot_y + 18
    desc = '; '.join(f'{l}: pg_sift {v["sift"]:,} QPS, ES {v["es"]:,}, ParadeDB {v["paradedb"]:,}, rum {v["rum"]:,}' for l, _, v in rows)
    return svg_wrap(height, title, desc, body)


def render_storage_svg(data):
    title = 'On-Disk Storage Footprint: 10M Orders & 50K Wikipedia BM25 Index'
    sub = data['storage_meta']['subtitle']
    foot = data['storage_meta']['footnote']
    orders = data['storage_orders']
    bm25 = data['storage_bm25']

    left = 192
    right = 268
    pw = WIDTH - left - right
    rh = 24
    bar_h = 14

    body = [
        f'  <text class="title" x="24" y="34" font-size="16">{escape(title)}</text>',
        f'  <text class="sub" x="24" y="53" font-size="11.5">{escape(sub)}</text>',
    ]

    # Panel A: 10M orders
    top_a = 84
    body.append(f'  <text class="sec" x="24" y="{top_a - 10}" font-size="12">A. 10,000,000 Orders — Total On-Disk Size (MiB)</text>')
    max_a = max(m for _, m, _, _ in orders)
    hi_a = math.ceil(max_a / 400.0) * 400.0
    Xa = lambda v: left + (v / hi_a) * pw
    plot_ha = len(orders) * rh
    for t in range(0, int(hi_a) + 1, 400):
        tx = Xa(t)
        body.append(f'  <line class="grid" x1="{fnum(tx)}" y1="{top_a}" x2="{fnum(tx)}" y2="{top_a + plot_ha}" stroke-width="0.8"/>')
        body.append(f'  <text class="tick" x="{fnum(tx)}" y="{top_a + plot_ha + 14}" font-size="10" text-anchor="middle">{t:,}</text>')
    body.append(f'  <line class="axis" x1="{left}" y1="{top_a}" x2="{left}" y2="{top_a + plot_ha}" stroke-width="1"/>')
    for i, (label, mib, ckey, note) in enumerate(orders):
        y = top_a + i * rh
        bw = max(4.0, Xa(mib) - left)
        lcls = 'label' if ckey in ('sift', 'sift_table') else 'sublabel'
        body.append(f'  <text class="{lcls}" x="{left - 10}" y="{fnum(y + 15)}" font-size="11.5" text-anchor="end">{escape(label)}</text>')
        body.append(f'  <rect class="c-{ckey}" x="{left}" y="{fnum(y + 4)}" width="{fnum(bw)}" height="{bar_h}" rx="3"/>')
        body.append(f'  <text class="val" x="{fnum(left + bw + 7)}" y="{fnum(y + 15)}" font-size="11">{escape(fmt_mib(mib))}</text>')
        body.append(f'  <text class="sublabel" x="{WIDTH - 24}" y="{fnum(y + 15)}" font-size="10" text-anchor="end">{escape(note)}</text>')

    # Panel B: 50K Wikipedia BM25 Index Size
    top_b = top_a + plot_ha + 46
    body.append(f'  <line class="sep" x1="24" y1="{top_b - 24}" x2="{WIDTH - 24}" y2="{top_b - 24}" stroke-width="0.8"/>')
    body.append(f'  <text class="sec" x="24" y="{top_b - 10}" font-size="12">B. 50,000 Wikipedia Articles — BM25 Index Size (MiB)</text>')
    max_b = max(m for _, m, _, _ in bm25)
    hi_b = math.ceil(max_b / 20.0) * 20.0
    Xb = lambda v: left + (v / hi_b) * pw
    plot_hb = len(bm25) * rh
    for t in range(0, int(hi_b) + 1, 20):
        tx = Xb(t)
        body.append(f'  <line class="grid" x1="{fnum(tx)}" y1="{top_b}" x2="{fnum(tx)}" y2="{top_b + plot_hb}" stroke-width="0.8"/>')
        body.append(f'  <text class="tick" x="{fnum(tx)}" y="{top_b + plot_hb + 14}" font-size="10" text-anchor="middle">{t}</text>')
    body.append(f'  <line class="axis" x1="{left}" y1="{top_b}" x2="{left}" y2="{top_b + plot_hb}" stroke-width="1"/>')
    for i, (label, mib, ckey, note) in enumerate(bm25):
        y = top_b + i * rh
        bw = max(4.0, Xb(mib) - left)
        lcls = 'label' if ckey in ('sift', 'sift_table') else 'sublabel'
        body.append(f'  <text class="{lcls}" x="{left - 10}" y="{fnum(y + 15)}" font-size="11.5" text-anchor="end">{escape(label)}</text>')
        body.append(f'  <rect class="c-{ckey}" x="{left}" y="{fnum(y + 4)}" width="{fnum(bw)}" height="{bar_h}" rx="3"/>')
        body.append(f'  <text class="val" x="{fnum(left + bw + 7)}" y="{fnum(y + 15)}" font-size="11">{escape(fmt_mib(mib))}</text>')
        body.append(f'  <text class="sublabel" x="{WIDTH - 24}" y="{fnum(y + 15)}" font-size="10" text-anchor="end">{escape(note)}</text>')

    foot_y = top_b + plot_hb + 36
    body.append(f'  <text class="sub" x="24" y="{foot_y}" font-size="9.8">{escape(foot)}</text>')
    height = foot_y + 18
    desc = '10M orders: ' + ', '.join(f'{l} {fmt_mib(m)}' for l, m, _, _ in orders) + '; 50K Wikipedia BM25: ' + ', '.join(f'{l} {fmt_mib(m)}' for l, m, _, _ in bm25)
    return svg_wrap(height, title, desc, body)

# The line and bar series of the charts of 6.0: stroke and fill per series, light and dark
SERIES_STYLE = """
  <style>
    .s-ideal { stroke: #8c959f; }
    .s-a1 { stroke: #2563eb; }
    .d-a1 { fill: #2563eb; }
    .s-a2 { stroke: #059669; }
    .d-a2 { fill: #059669; }
    .s-a3 { stroke: #d97706; }
    .d-a3 { fill: #d97706; }
    .s-a4 { stroke: #7c3aed; }
    .d-a4 { fill: #7c3aed; }
    .s-a5 { stroke: #0d9488; }
    .d-a5 { fill: #0d9488; }
    .s-a6 { stroke: #ea580c; }
    .d-a6 { fill: #ea580c; }
    .c-io_uring { fill: #2563eb; }
    .c-worker { fill: #d97706; }
    .c-sync { fill: #64748b; }
    .c-v51 { fill: #64748b; }
    .c-v6 { fill: #2563eb; }
    @media (prefers-color-scheme: dark) {
      .s-ideal { stroke: #484f58; }
      .s-a1 { stroke: #3b82f6; }
      .d-a1 { fill: #3b82f6; }
      .s-a2 { stroke: #10b981; }
      .d-a2 { fill: #10b981; }
      .s-a3 { stroke: #f59e0b; }
      .d-a3 { fill: #f59e0b; }
      .s-a4 { stroke: #a78bfa; }
      .d-a4 { fill: #a78bfa; }
      .s-a5 { stroke: #14b8a6; }
      .d-a5 { fill: #14b8a6; }
      .s-a6 { stroke: #fb923c; }
      .d-a6 { fill: #fb923c; }
      .c-io_uring { fill: #3b82f6; }
      .c-worker { fill: #f59e0b; }
      .c-sync { fill: #94a3b8; }
      .c-v51 { fill: #94a3b8; }
      .c-v6 { fill: #3b82f6; }
    }
  </style>"""

WARM_PROBES = [('A1_scan_filter', 'a1', 'Scan + filter'), ('A2_lowcard_groupby', 'a2', 'Low-card GROUP BY'), ('A3_highcard_groupby', 'a3', 'High-card GROUP BY'),
               ('A4_count_distinct', 'a4', 'count(DISTINCT)'), ('A5_parallel_build', 'a5', 'CREATE INDEX'), ('A6_parallel_vacuum', 'a6', 'VACUUM')]
COLD_PROBES = [('B1_cold_wide_scan', 'Wide scan'), ('B2_cold_narrow_scan', '2 of 15 columns'), ('B3_cold_table_bitmap', 'Table bitmap scan'), ('B4_cold_bm25', 'BM25, 8 segments')]
COLD_METHODS = [('io_uring', 'io_uring'), ('worker', 'worker'), ('sync', 'sync')]
NOT_MEASURED = 'not measured yet'


def fmt_rate(v):
    return f'{v / 1000:.1f} GB/s' if v >= 1000 else f'{v:.0f} MB/s'


def placeholder(body, x, y, text):
    body.append(f'  <text class="sub" x="{x}" y="{y}" font-size="11.5" text-anchor="middle">{escape(text)}</text>')


def render_scaling_svg(data):
    title = 'Parallel Scaling and Cold I/O: 1 to 8 Workers, io_uring, worker and sync'
    sc = data['scaling']
    meta = sc['meta'] if sc else {}
    sub = 'Warm speedup of the pg_sift executor against ideal linear (left) · cold-cache read throughput by io_method, read stream on (right)'
    foot = ('Data: docs/results/v6-scaling.tsv · %s · %s · PostgreSQL %s · %s' % (meta.get('cpu', '?'), meta.get('storage', '?'), meta.get('pg', '?'), meta.get('date', '?'))
            if sc else 'docs/results/v6-scaling.tsv is %s: `bench/bench scaling-io --docs` writes it.' % NOT_MEASURED)
    body = [
        f'  <text class="title" x="24" y="34" font-size="16">{escape(title)}</text>',
        f'  <text class="sub" x="24" y="53" font-size="11.5">{escape(sub)}</text>',
    ]
    px, pw, top, ph = 64, 360, 104, 230
    body.append(f'  <text class="sec" x="24" y="{top - 14}" font-size="12">A. Warm compute: speedup over 1 worker</text>')
    warm = {p: sc['warm'][p] for p, _, _ in WARM_PROBES if sc and p in sc['warm']}
    workers = sorted({w for v in warm.values() for w in v}) or [1, 2, 4, 8]
    wmax = max(workers)
    X = lambda w: px + math.log2(w) / math.log2(wmax) * pw if wmax > 1 else px   # noqa: E731
    Y = lambda v: top + ph - v / wmax * ph   # noqa: E731
    for t in workers:
        body.append(f'  <line class="grid" x1="{fnum(X(t))}" y1="{top}" x2="{fnum(X(t))}" y2="{top + ph}" stroke-width="0.8"/>')
        body.append(f'  <text class="tick" x="{fnum(X(t))}" y="{top + ph + 15}" font-size="10" text-anchor="middle">{t}</text>')
    body.append(f'  <text class="tick" x="{fnum(px + pw / 2)}" y="{top + ph + 30}" font-size="10" text-anchor="middle">workers</text>')
    for t in sorted({1, wmax // 2, wmax} - {0}):
        body.append(f'  <line class="grid" x1="{px}" y1="{fnum(Y(t))}" x2="{px + pw}" y2="{fnum(Y(t))}" stroke-width="0.8"/>')
        body.append(f'  <text class="tick" x="{px - 8}" y="{fnum(Y(t) + 3.5)}" font-size="10" text-anchor="end">{t}×</text>')
    body.append(f'  <line class="axis" x1="{px}" y1="{top}" x2="{px}" y2="{top + ph}" stroke-width="1"/>')
    body.append(f'  <line class="s-ideal" x1="{fnum(X(1))}" y1="{fnum(Y(1))}" x2="{fnum(X(wmax))}" y2="{fnum(Y(wmax))}" stroke-width="1.2" stroke-dasharray="5 4"/>')
    ly = top + ph + 50
    body.append(f'  <line class="s-ideal" x1="{px}" y1="{ly - 4}" x2="{px + 22}" y2="{ly - 4}" stroke-width="1.2" stroke-dasharray="5 4"/>')
    body.append(f'  <text class="label" x="{px + 28}" y="{ly}" font-size="10.5">ideal linear</text>')
    ends = []
    for i, (probe, cls, label) in enumerate(p for p in WARM_PROBES if p[0] in warm):
        pts = [(X(w), Y(v)) for w, v in sorted(warm[probe].items())]
        body.append(f'  <polyline class="s-{cls}" points="{" ".join(f"{fnum(x)},{fnum(y)}" for x, y in pts)}" fill="none" stroke-width="2" stroke-linejoin="round"/>')
        body += [f'  <circle class="d-{cls}" cx="{fnum(x)}" cy="{fnum(y)}" r="3"/>' for x, y in pts]
        ends.append((pts[-1][1], f'{warm[probe][max(warm[probe])]:.1f}×'))
        lrow, lcol = divmod(i, 3)
        body.append(f'  <rect class="d-{cls}" x="{px + lcol * 150}" y="{ly + 10 + lrow * 16}" width="10" height="10" rx="2"/>')
        body.append(f'  <text class="label" x="{px + lcol * 150 + 15}" y="{ly + 19 + lrow * 16}" font-size="10">{escape(label)}</text>')
    last_y = -99.0
    for y, text in sorted(ends):
        last_y = max(y, last_y + 11)
        body.append(f'  <text class="val" x="{fnum(X(wmax) + 8)}" y="{fnum(last_y + 3.5)}" font-size="10">{text}</text>')
    if not warm:
        placeholder(body, px + pw / 2, top + ph / 2, 'warm scaling ' + NOT_MEASURED)

    bx, bw = 500, 336
    body.append(f'  <text class="sec" x="{bx}" y="{top - 14}" font-size="12">B. Cold I/O: read throughput (MB/s)</text>')
    cold = {p: sc['cold'][p] for p, _ in COLD_PROBES if sc and p in sc['cold']}
    vmax = max([v for at in cold.values() for v in at.values()] or [1])
    hi = 10 ** math.ceil(math.log10(vmax)) if vmax > 0 else 1
    hi = hi / 2 if vmax <= hi / 2 else hi
    gh, gap = 3 * 11 + 2 * 2, 18
    for t in (0, hi / 2, hi) if cold else ():
        gx = bx + 96 + t / hi * (bw - 96 - 52)
        body.append(f'  <line class="grid" x1="{fnum(gx)}" y1="{top}" x2="{fnum(gx)}" y2="{top + ph}" stroke-width="0.8"/>')
        body.append(f'  <text class="tick" x="{fnum(gx)}" y="{top + ph + 15}" font-size="10" text-anchor="middle">{t:,.0f}</text>')
    body.append(f'  <line class="axis" x1="{bx + 96}" y1="{top}" x2="{bx + 96}" y2="{top + ph}" stroke-width="1"/>')
    for gi, (probe, label) in enumerate(p for p in COLD_PROBES if p[0] in cold):
        gy = top + 8 + gi * (gh + gap)
        body.append(f'  <text class="label" x="{bx + 88}" y="{fnum(gy + gh / 2 + 3)}" font-size="10.5" text-anchor="end">{escape(label)}</text>')
        for si, (method, _) in enumerate(COLD_METHODS):
            if method not in cold[probe]:
                continue
            v = cold[probe][method]
            w = max(3.0, v / hi * (bw - 96 - 52))
            by = gy + si * 13
            body.append(f'  <rect class="c-{method}" x="{bx + 96}" y="{fnum(by)}" width="{fnum(w)}" height="11" rx="2.5"/>')
            body.append(f'  <text class="val-muted" x="{fnum(bx + 96 + w + 5)}" y="{fnum(by + 9)}" font-size="10">{escape(fmt_rate(v))}</text>')
    if cold:
        draw_legend(body, bx, ly + 4, COLD_METHODS)
    else:
        placeholder(body, bx + bw / 2, top + ph / 2, 'cold I/O ' + NOT_MEASURED)

    foot_y = ly + 62
    body.append(f'  <text class="sub" x="24" y="{foot_y}" font-size="9.8">{escape(foot)}</text>')
    desc = 'Warm speedup at %d workers: ' % wmax + ', '.join(f'{l} {warm[p][max(warm[p])]:.1f}x' for p, _, l in WARM_PROBES if p in warm) + '. Cold read throughput: ' + \
        '; '.join(f'{l}: ' + ', '.join(f'{m} {fmt_rate(cold[p][m])}' for m, _ in COLD_METHODS if m in cold[p]) for p, l in COLD_PROBES if p in cold)
    return svg_wrap(foot_y + 18, title, desc if (warm or cold) else title + ': ' + NOT_MEASURED, body, SERIES_STYLE)


def render_memory_svg(data):
    title = 'Memory Stays Flat: Scoped Index Build, 5.1 against 6.0'
    mem = data['memory']
    base, now, meta = mem['v51'], mem['v6'], mem['meta']
    sub = 'Peak backend RssAnon of a scoped CREATE INDEX CONCURRENTLY over the scale ladder (left) · index size and WAL in bytes a row at the top scale (right)'
    foot = ('Data: docs/results/v6-core.tsv · %s · PostgreSQL %s · %s · 5.1 from bench/baselines/v510.tsv' % (meta.get('cpu', '?'), meta.get('pg', '?'), meta.get('date', '?'))
            if now else '6.0 is %s: `bench/bench v6-core --docs` writes docs/results/v6-core.tsv; 5.1 from bench/baselines/v510.tsv.' % NOT_MEASURED)
    body = [
        f'  <text class="title" x="24" y="34" font-size="16">{escape(title)}</text>',
        f'  <text class="sub" x="24" y="53" font-size="11.5">{escape(sub)}</text>',
    ]
    series = [('v51', '5.1')] + ([('v6', '6.0')] if now else [])
    ladders = {'v51': base, 'v6': now or {}}
    draw_legend(body, 24, 78, series)
    scales = sorted(set(base) | set(now or {}))

    def panel(x, w, title_text, cats, value, fmt):
        top, ph = 126, 200
        body.append(f'  <text class="sec" x="{x}" y="{top - 20}" font-size="12">{escape(title_text)}</text>')
        vmax = max([value(s, c) for s, _ in series for c, _ in cats if value(s, c) is not None] or [1])
        hi = 10 ** math.floor(math.log10(vmax)) * math.ceil(vmax / 10 ** math.floor(math.log10(vmax)))
        for t in (0, hi / 2, hi):
            y = top + ph - t / hi * ph
            body.append(f'  <line class="grid" x1="{x}" y1="{fnum(y)}" x2="{x + w}" y2="{fnum(y)}" stroke-width="0.8"/>')
            body.append(f'  <text class="tick" x="{x - 6}" y="{fnum(y + 3.5)}" font-size="10" text-anchor="end">{t:,.0f}</text>')
        cw = w / len(cats)
        bar = min(34.0, cw / (len(series) + 1))
        for ci, (c, clabel) in enumerate(cats):
            cx = x + ci * cw + cw / 2
            body.append(f'  <text class="tick" x="{fnum(cx)}" y="{top + ph + 15}" font-size="10.5" text-anchor="middle">{escape(clabel)}</text>')
            for si, (sk, _) in enumerate(series):
                v = value(sk, c)
                if v is None:
                    continue
                bx0 = cx - bar * len(series) / 2 + si * bar
                h = max(2.0, v / hi * ph)
                body.append(f'  <rect class="c-{sk}" x="{fnum(bx0 + 1)}" y="{fnum(top + ph - h)}" width="{fnum(bar - 2)}" height="{fnum(h)}" rx="2.5"/>')
                body.append(f'  <text class="val" x="{fnum(bx0 + bar / 2)}" y="{fnum(top + ph - h - 5)}" font-size="9.5" text-anchor="middle">{escape(fmt(v))}</text>')
        body.append(f'  <line class="axis" x1="{x}" y1="{top + ph}" x2="{x + w}" y2="{top + ph}" stroke-width="1"/>')

    last = scales[-1] if scales else None
    panel(64, 360, 'A. Peak RssAnon (MB) by rows', [(n, f'{n // 1000}k rows') for n in scales], lambda s, n: ladders[s].get(n, {}).get('rss'), lambda v: f'{v:.1f}')
    panel(530, 300, f'B. Bytes a row at {last // 1000 if last else 0}k rows', [('size', 'index size'), ('wal', 'WAL')], lambda s, c: ladders[s].get(last, {}).get(c), lambda v: f'{v:.0f}')
    foot_y = 126 + 200 + 56
    body.append(f'  <text class="sub" x="24" y="{foot_y}" font-size="9.8">{escape(foot)}</text>')
    desc = 'Peak RssAnon of the scoped index build by rows: ' + '; '.join(
        f'{s}: ' + ', '.join(f'{n // 1000}k {ladders[k][n]["rss"]:.1f} MB' for n in scales if n in ladders[k]) for k, s in series)
    return svg_wrap(foot_y + 18, title, desc, body, SERIES_STYLE)


def generate(root):
    results_dir = os.path.join(root, 'docs', 'results')
    out_dir = os.path.join(root, 'docs', 'charts')
    os.makedirs(out_dir, exist_ok=True)
    data = load_live_data(results_dir)
    charts = {
        'analytics-engines.svg': render_analytics_svg(data),
        'search-bm25.svg': render_search_svg(data),
        'storage-footprint.svg': render_storage_svg(data),
        'scaling-parallel-io.svg': render_scaling_svg(data),
        'memory-flatness.svg': render_memory_svg(data),
    }
    written = []
    for name, content in charts.items():
        path = os.path.join(out_dir, name)
        if not os.path.exists(path) or open(path, encoding='utf-8').read() != content:
            with open(path, 'w', encoding='utf-8', newline='\n') as f:
                f.write(content)
        written.append(path)
    return written


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--root', default=REPO)
    a = ap.parse_args()
    for p in generate(a.root):
        print(p)


if __name__ == '__main__':
    main()
