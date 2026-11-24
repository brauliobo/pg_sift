"""The numbers behind every chart: the audited committed baseline and the overlay of docs/results/ when its files exist.

A series nobody measured yet is absent (None or missing), never estimated: its chart says `not measured yet`. Baseline sources: store-scaling.json (10M-c3), store.json (1M-c3),
duckdb-10000000.tsv, articles.tsv (slim, slim-cold), scope-stock.tsv (v5.0, 2,400,007 events), v6-core.tsv and bench/baselines/v510.tsv (5.1), all committed on 2026-10-27.
"""
import csv
import json
import os
import re

HERE = os.path.dirname(os.path.abspath(__file__))
MEMORY_BASELINE = os.path.join(os.path.dirname(os.path.dirname(HERE)), 'baselines', 'v510.tsv')

LABEL = {'sift': 'pg_sift sift_col', 'sift_table': 'pg_sift sift_table', 'ch': 'ClickHouse', 'ch_proj': 'CH + PROJECTION', 'es': 'Elasticsearch 9.5', 'duckdb': 'DuckDB', 'pg': 'PG 18'}
ORDERS_ARMS = ('sift', 'sift_table', 'ch', 'ch_proj', 'es', 'duckdb', 'pg')
QUERY_ORDER_ANALYTICS = [('02', 'Count by country', 'count(*) GROUP BY country'), ('03', 'Revenue / month', 'sum(amount) GROUP BY month'),
                         ('04', 'Top 10 categories', 'sum(amount) GROUP BY LIMIT 10'), ('05', 'Avg shipped order', 'avg(amount) WHERE 3 filters'),
                         ('06', 'Distinct / month', 'count(DISTINCT customer_id)'), ('07', 'Big recent orders', 'count(*) WHERE range + amount')]
QUERY_SLUG = {'01': 'point-lookup', '02': 'orders-per-country', '03': 'revenue-per-month', '04': 'top-categories', '05': 'average-shipped-order', '06': 'customers-per-month',
              '07': 'big-recent-orders', '08': 'search-count', '09': 'search-facets', '10': 'search-newest'}
QUERY_ORDER_SEARCH = [('count', 'Exact count(*)', 'matching document count'), ('phrase', 'Phrase "a b"', 'exact proximity BM25 top-10'),
                      ('and', 'Boolean a AND b', '2-term conjunction BM25'), ('mixed', 'Mixed boolean', 'AND / OR / NOT BM25 top-10'),
                      ('filter', 'Filtered top-k', 'BM25 + INCLUDE filter pushdown'), ('or', 'Boolean a OR b', '2-term disjunction BM25'),
                      ('join', 'Joined top-k*', 'BM25 joined with dimension table')]
SEARCH_ARMS = {'sift': 'sift', 'es': 'elasticsearch', 'paradedb': 'pg_search', 'rum': 'rum', 'textsearch': 'pg_textsearch'}
CB_FAMILIES = [('Aggs', range(0, 8)), ('Group/Distinct', range(8, 19)), ('LIKE/Filter', [*range(19, 24), *range(36, 43)]), ('Top-N', range(24, 27)), ('Wide/High-Card', range(27, 36))]
SCOPE_STRUCTURE = {'sift_scope': 0, 'stock_default': 0, 'semijoin': 0, 'ch_join': 0, 'ch_proj': 0, 'denorm_flat': 1, 'denorm_part': 1, 'rollup': 1, 'pg_ivm': 2, 'ch_mv': 2}

WARM_10M = {  # qid: sift_col, sift_table, ClickHouse, Elasticsearch (approx HLL++ on 06), DuckDB Parquet, PG 18
    '02': (0.85, 1.65, 9.32, 6.0, 8.97, 188.90), '03': (0.93, 1.75, 23.79, 69.7, 99.17, 1377.23), '04': (0.90, 2.09, 20.96, 68.0, 26.67, 370.20),
    '05': (10.04, 9.87, 7.68, 28.9, 48.89, 124.24), '06': (49.34, 53.9, 104.54, 260.1, 193.70, 2333.82), '07': (10.63, 13.8, 4.39, 6.3, 39.73, 146.44)}
WARM_COLD_1M = {  # qid: (warm, cold) of sift_col, sift_table, ClickHouse, Elasticsearch, PG 18
    '02': ((0.56, 9.89), (0.54, 8.51), (4.95, 10.7), (6.00, 125.2), (22.0, 34.7)), '03': ((0.65, 2.90), (0.61, 3.26), (6.91, 7.18), (13.7, 215.0), (192.5, 197.7)),
    '04': ((0.67, 3.03), (0.62, 3.27), (6.12, 16.6), (11.5, 214.8), (43.4, 59.5)), '05': ((1.92, 16.9), (2.10, 18.0), (7.01, 6.57), (5.68, 182.1), (10.6, 25.8)),
    '06': ((6.16, 10.9), (8.71, 14.0), (14.0, 10.9), (45.9, 441.3), (293.4, 224.4)), '07': ((2.05, 7.79), (2.07, 9.95), (3.66, 3.77), (3.38, 89.5), (12.0, 28.3))}
QPS_50K = {  # class: sift, Elasticsearch, ParadeDB, PG rum, pg_textsearch
    'count': (29127, 28105, 2399, 229, 161), 'phrase': (21794, 19423, 2332, 184, 201), 'and': (21690, 17476, 2414, 4909, 172), 'mixed': (14159, 14749, 2248, 112, 147),
    'filter': (12604, 13854, 2241, 113, 163), 'or': (8201, 11021, 2000, 50, 9222), 'join': (6689, 14037, 431, 110, 189)}
COLD_P50_50K = {  # class: sift, Elasticsearch, ParadeDB, PG rum, pg_textsearch
    'count': (1.03, 3.11, 16.06, 31.29, 8.86), 'phrase': (1.31, 3.29, 14.80, 16.95, 8.08), 'and': (1.76, 4.24, 17.08, 3.57, 9.21), 'filter': (1.67, 3.88, 15.90, 17.30, 9.01),
    'or': (5.50, 4.77, 14.54, 56.90, 7.02), 'join': (1.75, 3.67, 13.12, 17.97, 8.84)}
SEARCH_10M = {'08': (2.35, 11.57, 78.53), '09': (13.69, 8.27, 80.44), '10': (13.13, 13.63, 141.68)}   # sift_col, Elasticsearch, PG GIN
ORDERS_SIZES = [('DuckDB (Parquet)', 297.3, 'duckdb', 'external file · no PK/BM25'), ('ClickHouse 26.9', 421.4, 'ch', 'MergeTree · no PK/BM25'),
                ('pg_sift (sift_table)', 456.6, 'sift_table', 'col + PK + BM25 (3.4× vs PG)'), ('Citus 14.2 columnar', 463.4, 'pg', 'col + PK · no BM25'),
                ('pg_sift (sift_col + heap)', 1437.5, 'sift', '1,023 MiB heap + 415 idx'), ('Elasticsearch 9.5', 1456.9, 'es', 'Lucene index + _source'),
                ('PostgreSQL 18 (stock)', 1549.1, 'pg', '1,023 MiB heap + 526 idx')]
BM25_SIZES = [('pg_sift (positions=off)', 12.0, 'sift_table', '0.6 s build'), ('pg_sift (default)', 30.2, 'sift', '0.7 s · full positions'),
              ('pg_textsearch 1.5', 40.1, 'duckdb', '5.1 s build'), ('ParadeDB (pg_search)', 54.5, 'paradedb', '0.6 s build'),
              ('Elasticsearch 9.5', 70.5, 'es', '3.3 s + 1.3 s merge'), ('PG rum (tsvector)', 85.7, 'pg', '3.8 s (+100 MiB table)')]
SCOPES = {'cells': 72, 'read_ms': {'sift_scope': 2.18, 'stock_default': 542.0, 'semijoin': 285.0, 'denorm_flat': 329.0, 'pg_ivm': 43.3},
          'tally': {'stock_default': (72, 0, 0), 'semijoin': (63, 2, 7), 'denorm_flat': (51, 2, 19), 'denorm_part': (54, 4, 14), 'rollup': (54, 5, 5), 'pg_ivm': (32, 2, 30)}}
SCOPE_BUILD = [('plain scoped CREATE INDEX', '1.79× entity', '≤ 1.15×'), ('scoped CIC pass 2', '107% of pass 1', '≤ 15%'),
               ('scope build temp I/O', '249 B/row', '0 B'), ('scope disk vs entity', '+98.3%', '≤ 5%')]
WAL = [('1-row INSERT WAL', 460, 495.7, 220), ('2% DELETE WAL', 60, 98.7, 45), ('bulk build WAL', None, 111.8, 45)]   # B/row: PG 18 heap, pg_sift 5.1 measured, 6.0 gate


def baseline():
    store = {q: dict(zip(('sift', 'sift_table', 'ch', 'es', 'duckdb', 'pg'), v)) for q, v in WARM_10M.items()}
    warm_cold = {q: dict(zip(('sift', 'sift_table', 'ch', 'es', 'pg'), v)) for q, v in WARM_COLD_1M.items()}
    return {
        'orders_tier': '10M', 'orders_rows': 10_000_000,
        'analytics': [(label, sub, store[q]) for q, label, sub in QUERY_ORDER_ANALYTICS], 'analytics_cold': {QUERY_SLUG[q]: v for q, v in warm_cold.items()},
        'search': [(label, sub, dict(zip(SEARCH_ARMS, QPS_50K[c]))) for c, label, sub in QUERY_ORDER_SEARCH],
        'search_cold': {c: dict(zip(SEARCH_ARMS, v)) for c, v in COLD_P50_50K.items()},
        'search_10m': {q: dict(zip(('sift', 'es', 'pg'), v)) for q, v in SEARCH_10M.items()},
        'storage_orders': list(ORDERS_SIZES), 'storage_bm25': list(BM25_SIZES),
        'scoped_rollup': {'rows': 2400007, 'sift_ms': 2.18, 'stock_ms': 542.0, 'faster_cells': 72, 'cells': 72, 'version': 'v5.0'},
        'scopes': {k: dict(v) if isinstance(v, dict) else v for k, v in SCOPES.items()}, 'scope_build': SCOPE_BUILD, 'wal': WAL,
        'clickbench': None, 'scaling': None, 'memory': load_memory(None)}


def read_tsv(path):
    """(meta, rows) of a tab separated file with `# key=value` meta lines and a header row; ({}, []) when the file is not there"""
    if not path or not os.path.exists(path):
        return {}, []
    with open(path, encoding='utf-8') as f:
        lines = [l.rstrip('\n') for l in f if l.strip()]
    meta = dict(l[2:].split('=', 1) for l in lines if l.startswith('# ') and '=' in l)
    return meta, list(csv.DictReader([l for l in lines if not l.startswith('#')], delimiter='\t'))


def tier_rows(name):
    m = re.match(r'(\d+)([kKmM])', name)
    return int(m.group(1)) * {'k': 10 ** 3, 'm': 10 ** 6}[m.group(2).lower()] if m else 0


def load_store(results_dir):
    """{tier: (meta, rows)} of the store results (store-scaling.json and store.json), the tiers by name"""
    tiers = {}
    for name in ('store.json', 'store-scaling.json'):
        path = os.path.join(results_dir, name)
        if os.path.exists(path):
            with open(path, encoding='utf-8') as f:
                doc = json.load(f)
            for r in doc.get('rows', []):
                tiers.setdefault(r.get('tier'), (doc.get('meta', {}).get('tiers', {}).get(r.get('tier'), {}), []))[1].append(r)
    return tiers


def latency(rows, eng, slug, variant='warm'):
    return next((float(r['value']) for r in rows if r['engine'] == eng and r['class'] == slug and r['variant'] == variant and r['metric'] == 'latency_ms'), None)


def live_analytics(data, rows, duckdb):
    got = [(label, sub, {e: v for e in ORDERS_ARMS for v in [duckdb.get(QUERY_SLUG[q]) if e == 'duckdb' else latency(rows, e, QUERY_SLUG[q])] if v is not None})
           for q, label, sub in QUERY_ORDER_ANALYTICS]
    if all(all(e in vals for e in ('sift', 'ch', 'duckdb', 'pg')) for _, _, vals in got):
        data['analytics'] = [(l, s, {e: round(v, 2) for e, v in vals.items()}) for l, s, vals in got]
    for q, qd in data['search_10m'].items():
        live = {e: latency(rows, e, QUERY_SLUG[q]) for e in qd}
        if None not in live.values():
            qd.update(live)


def live_sizes(data, rows, duckdb_mib):
    size = lambda eng, variant='-': next((float(r['value']) for r in rows if r['engine'] == eng and r['metric'] == 'size_mib' and r['variant'] == variant), None)  # noqa: E731
    if any(size(e) is None for e in ('ch', 'sift_table', 'citus', 'sift', 'es', 'pg')):
        return
    note = lambda eng: f'{size(eng, "table"):,.0f} MiB heap + {size(eng) - size(eng, "table"):,.0f} idx'  # noqa: E731
    items = [('ClickHouse 26.9', size('ch'), 'ch', 'MergeTree · no PK/BM25'), ('pg_sift (sift_table)', size('sift_table'), 'sift_table', f'col + PK + BM25 ({size("pg") / size("sift_table"):.1f}× vs PG)'),
             ('Citus 14.2 columnar', size('citus'), 'pg', 'col + PK · no BM25'), ('pg_sift (sift_col + heap)', size('sift'), 'sift', note('sift')),
             ('Elasticsearch 9.5', size('es'), 'es', 'Lucene index + _source'), ('PostgreSQL 18 (stock)', size('pg'), 'pg', note('pg'))]
    data['storage_orders'] = sorted(items + ([('DuckDB (Parquet)', duckdb_mib, 'duckdb', 'external file · no PK/BM25')] if duckdb_mib else []), key=lambda x: x[1])


def load_duckdb(path):
    _, rows = read_tsv(path)
    warm = {r['case']: float(r['value']) for r in rows if r['arm'] == 'duckdb_parquet' and r['metric'] == 'warm_ms'}
    mib = next((float(r['value']) for r in rows if r['arm'] == 'duckdb_parquet' and r['metric'] == 'table_mib'), None)
    return warm, mib


def live_orders(data, results_dir):
    tiers = load_store(results_dir)
    if not tiers:
        return
    top = max(tiers, key=tier_rows)
    data['orders_tier'], data['orders_rows'] = re.match(r'\d+[kKmM]', top).group(0), tier_rows(top)
    warm, mib = load_duckdb(os.path.join(results_dir, f'duckdb-{data["orders_rows"]}.tsv'))
    live_analytics(data, tiers[top][1], warm)
    live_sizes(data, tiers[top][1], mib)
    mid = next((t for t in tiers if tier_rows(t) == 1_000_000), None)
    for q, _, _ in QUERY_ORDER_ANALYTICS if mid else ():
        slug = QUERY_SLUG[q]
        row = {e: (latency(tiers[mid][1], e, slug), latency(tiers[mid][1], e, slug, 'cold')) for e in ORDERS_ARMS}
        data['analytics_cold'][slug] = {e: v for e, v in row.items() if None not in v}


def live_search(data, results_dir):
    _, rows = read_tsv(os.path.join(results_dir, 'articles.tsv'))
    slim = [r for r in rows if r.get('tier') == 'slim' and r.get('variant') == '-']
    value = lambda rs, eng, cls, metric: next((float(r['value']) for r in rs if r['engine'] == eng and r['class'] == cls and r['metric'] == metric and r['value'] not in ('NA', 'error')), None)  # noqa: E731
    got = [(l, s, {k: round(v) for k, e in SEARCH_ARMS.items() for v in [value(slim, e, c, 'qps')] if v is not None}) for c, l, s in QUERY_ORDER_SEARCH]
    if all(all(k in vals for k in ('sift', 'es', 'paradedb', 'rum')) for _, _, vals in got):
        data['search'] = got
    cold = [r for r in rows if r.get('tier') == 'slim-cold' and r.get('variant') == '-']
    live_cold = {c: {k: v for k, e in SEARCH_ARMS.items() for v in [value(cold, e, c, 'cold_p50_ms')] if v is not None} for c, _, _ in QUERY_ORDER_SEARCH}
    data['search_cold'].update({c: v for c, v in live_cold.items() if 'sift' in v and 'es' in v})
    size, build = ({r['engine']: float(r['value']) for r in slim if r['metric'] == m and r['value'] not in ('NA', 'error')} for m in ('index_mib', 'build_s'))
    if all(e in size for e in ('sift_nopos', 'sift', 'pg_textsearch', 'pg_search', 'elasticsearch', 'rum')):
        data['storage_bm25'] = [('pg_sift (positions=off)', size['sift_nopos'], 'sift_table', f'{build.get("sift_nopos", 0.6):.1f} s build'),
                                ('pg_sift (default)', size['sift'], 'sift', f'{build.get("sift", 0.7):.1f} s · full positions'),
                                ('pg_textsearch 1.5', size['pg_textsearch'], 'duckdb', f'{build.get("pg_textsearch", 5.1):.1f} s build'),
                                ('ParadeDB (pg_search)', size['pg_search'], 'paradedb', f'{build.get("pg_search", 0.6):.1f} s build'),
                                ('Elasticsearch 9.5', size['elasticsearch'], 'es', f'{build.get("elasticsearch", 3.3):.1f} s + 1.3 s merge'),
                                ('PG rum (tsvector)', size['rum'], 'pg', f'{build.get("rum", 3.8):.1f} s (+100 MiB table)')]


def load_scopes(path):
    """(scoped rollup row, scopes) of docs/results/scope-stock.tsv (tenant 1 `sum_by_month` read of each arm, the warm tally of each rival), None without the file or its cells"""
    if not os.path.exists(path):
        return None
    import scope_stock_tables as sst
    R = sst.Results(path)
    read = {arm: R.get(1, 'sum_by_month', arm, 'warm_ms') for arm in R.arms}
    if read.get(sst.BASE) is None or read.get('stock_default') is None:
        return None
    cells, faster, _, _, _ = sst.tally_row(R, 'stock_default', 'warm_ms')
    tally = {a: sst.tally_row(R, a, 'warm_ms')[1:4] for a in R.arms if a != sst.BASE}
    rollup = {'rows': int(R.meta['facts']), 'sift_ms': read[sst.BASE], 'stock_ms': read['stock_default'], 'faster_cells': faster, 'cells': cells, 'version': 'v%s' % R.meta['pg_sift']}
    return rollup, {'cells': cells, 'read_ms': {a: v for a, v in read.items() if v is not None}, 'tally': tally}


def load_scaling(path):
    """{'meta', 'warm': {probe: {workers: speedup}}, 'cold': {probe: {io_method: MB/s}}, 'writers': {clients: ops/s}} of docs/results/v6-scaling.tsv (the cold rows: the stream on, the most workers)"""
    meta, rows = read_tsv(path)
    if not rows:
        return None
    warm, cold, writers = {}, {}, {}
    for r in rows:
        if r['suite'] == 'A' and r['speedup']:
            warm.setdefault(r['probe'], {})[int(r['workers'])] = float(r['speedup'])
        elif r['suite'] == 'B' and r['read_stream'] == 'on' and r['throughput'] and r['gate_pass'] != 'notcold':
            at = cold.setdefault(r['probe'], {})
            if r['io_method'] not in at or int(r['workers']) > at[r['io_method']][0]:
                at[r['io_method']] = (int(r['workers']), float(r['throughput']))
        elif r['suite'] == 'C' and r['probe'] == 'C1_multi_partition_writes' and r['throughput']:
            writers[int(r['workers'])] = float(r['throughput'])
    return {'meta': meta, 'warm': warm, 'cold': {p: {m: v[1] for m, v in at.items()} for p, at in cold.items()}, 'writers': writers}


def load_memory(path):
    """{'v51': ladder, '6.0': ladder or None, 'meta'} with ladder = {rows: {'rss': MB, 'size': B/row, 'wal': B/row}} of the scoped CIC build (section A_build_cic)"""
    def ladder(file):
        meta, rows = read_tsv(file)
        got = {}
        for r in rows:
            if r['section'] == 'A_build_cic' and r['scale'].isdigit():
                key = {'peak_rss_anon_mb': 'rss', 'size_mb': 'size', 'wal_mb': 'wal'}.get(r['metric'])
                if key:
                    got.setdefault(int(r['scale']), {})[key] = float(r['value']) if key == 'rss' else float(r['value']) * 1048576 / int(r['scale'])
        return meta, {k: v for k, v in got.items() if len(v) == 3}
    _, base = ladder(MEMORY_BASELINE)
    meta, now = ladder(path)
    return {'v51': base, 'v6': now or None, 'meta': meta}


def load_clickbench(path):
    """{'meta', 'arms', 'warm': {arm: {query: ms}}, 'equal': {arm: md5_equal count}} of docs/results/clickbench.tsv, None without it"""
    if not os.path.exists(path):
        return None
    import resultfile
    meta, rows = resultfile.read(path)
    ok = lambda r: r['value'] not in ('', 'NA')  # noqa: E731
    arms = sorted({r['arm'] for r in rows})
    warm = {a: {int(r['case'][1:]): float(r['value']) for r in rows if r['arm'] == a and r['metric'] == 'warm_ms' and ok(r)} for a in arms}
    equal = {a: sum(1 for r in rows if r['arm'] == a and r['metric'] == 'md5_equal' and r['value'] == '1') for a in arms}
    return {'meta': meta, 'arms': arms, 'warm': warm, 'equal': equal}


def load_live_data(results_dir):
    """the baseline with the files of results_dir laid over it (all of the baseline without a directory)"""
    data = baseline()
    if not results_dir or not os.path.isdir(results_dir):
        return data
    live_orders(data, results_dir)
    live_search(data, results_dir)
    scopes = load_scopes(os.path.join(results_dir, 'scope-stock.tsv'))
    if scopes:
        data['scoped_rollup'], data['scopes'] = scopes
    data['scaling'] = load_scaling(os.path.join(results_dir, 'v6-scaling.tsv'))
    data['memory'] = load_memory(os.path.join(results_dir, 'v6-core.tsv'))
    data['clickbench'] = load_clickbench(os.path.join(results_dir, 'clickbench.tsv'))
    return data
