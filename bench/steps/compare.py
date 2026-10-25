#!/usr/bin/env python3
"""The README's comparison with the other systems, from the committed results (bench/results, sqlite/README.md, mariadb/README.md), standard library only.

    python3 -I bench/steps/compare.py [--results DIR] [--readme FILE]      (steps/highlights.py runs it)

Rewrites <!-- bench:compare:begin --> (a matrix of workload classes against every other system: on how many questions pg_sift is faster and by how much, the losses by name,
`not measured` where no result is stored), compare-sources (the conditions of each number), compare-search (the search engines), compare-ports (SQLite and MariaDB) and
<!-- bench:numbers:begin --> (the warm milliseconds of every question on every system, at 1,000,000 and at 10,000,000 orders). A competitor figure is read from a results file or not shown: nothing here is typed, extrapolated or rerun.
"""
import argparse
import os
import re
import statistics
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import aggregates_page  # noqa: E402  (steps/aggregates_page.py: the shapes of analytics.json)
import bench_charts  # noqa: E402  (steps/bench_charts.py: the tenants, the pg_duckdb runs, lat)
import cases as cases_mod  # noqa: E402
import cases_render  # noqa: E402
import extras  # noqa: E402
import readme  # noqa: E402  (steps/readme.py: the search tier, replace_block)
import store_charts  # noqa: E402

REPO = os.path.dirname(os.path.dirname(HERE))
TIE = 0.05
MINE = ('sift', 'sift_table')
SLUG = store_charts.question_slugs()
NOT = 'not measured'
LABEL = {'pg': 'PostgreSQL', 'ch': 'ClickHouse', 'es': 'Elasticsearch', 'citus': 'Citus columnar', 'duckdb': 'pg_duckdb'}


def x(v):
    return f'{v:.0f}x' if v >= 9.5 else f'{v:.1f}x'


def cell(pairs, noun='faster'):
    """pairs [(name, other / pg_sift)]: '5 of 7 (1.8x to 21x); slower on revenue (1.2x)'; a tie (within 5%) is neither"""
    if not pairs:
        return NOT
    faster = [r for _, r in pairs if r >= 1 + TIE]
    slower = sorted(((n, 1 / r) for n, r in pairs if r <= 1 - TIE), key=lambda t: -t[1])
    ties = len(pairs) - len(faster) - len(slower)
    out = f'{len(faster)} of {len(pairs)}' + (f' ({x(min(faster))} to {x(max(faster))})' if len(faster) > 1 and x(min(faster)) != x(max(faster)) else f' ({x(faster[0])})' if faster else '')
    if ties:
        out += f', {ties} tie'
    if slower:
        shown = ', '.join(f'{n} {x(r)}' for n, r in slower[:3]) + (f' and {len(slower) - 3} more' if len(slower) > 3 else '')
        out += f'; **slower on {shown}**'
    return out


def single(ratio, better, worse):
    """one measure (a rate, a load time, a size): '3.4x smaller', '**1.2x larger**' or 'tie'"""
    return f'{x(ratio)} {better}' if ratio >= 1 + TIE else f'**{x(1 / ratio)} {worse}**' if ratio <= 1 - TIE else 'tie'


def best_mine(S, tier, q):
    v = [S.lat(tier, e, q, 'warm')['value'] for e in MINE if S.lat(tier, e, q, 'warm')]
    return min(v) if v else None


def store_pairs(S, D, tier, e, qs):
    """[(question, other / pg_sift)] on the questions the system answered; pg_duckdb against the pg_sift of its own run (its own client and PostgreSQL build)"""
    out = []
    for q in qs:
        if e == 'duckdb':
            theirs, mine = (D.best(q) if D else None), (D.own(q, 'sift') if D else None)
        else:
            r = S.lat(tier, e, q, 'warm')
            theirs, mine = (r['value'] if r else None), best_mine(S, tier, q)
        if theirs and mine:
            out.append((SLUG[q], theirs / mine))
    return out


def shape_pairs(results, what):
    found = aggregates_page.shape_cases(results)
    if not found:
        return {}
    cases = found[1]
    out = {'pg': [], 'ch': []}
    for q, v, t in cases:
        if what == 'whole' and v != '-' or what == 'set' and v == '-':
            continue
        name = q if v == '-' else f'{q} {v.replace("K", "")} tenants'
        for e, key in (('pg', 'heap'), ('ch', 'ch')):
            if t['col'] and t[key]:
                out[e].append((name, t[key] / t['col']))
    return out


GENERIC = (('Generic suite, TPC-H-like (5 queries, 1,000,000 lineitem rows, generated)', 'tpch_'), ('Generic suite, ClickBench-like (10 queries, a generated wide table of 1,000,000 rows)', 'hits_'),
           ('Generic suite, time series (4 queries, 1,000,000 readings)', 'ts_'), ('Write overhead of a `sift_col` index on a heap table (20,000 single-row INSERTs, 20,000 UPDATEs)', 'oltp_'))


def case_pairs(results, prefix=None):
    """([(case, PostgreSQL / pg_sift)], planner kept PostgreSQL's plan): the cases of one generic suite (prefix), or every other case"""
    path = os.path.join(results, 'cases.tsv')
    if not os.path.exists(path):
        return []
    figs = [f for f in cases_render.collect(extras.read(path)[1], cases_mod.read_registry()) if f['metric'] == 'ms' and f['native'] and f['sift']
            and (f['case'].id.startswith(prefix) if prefix else not f['case'].id.startswith(tuple(p for _, p in GENERIC)))]
    return [(f['case'].id, f['native'] / f['sift']) for f in figs if not f['kept']], sum(1 for f in figs if f['kept'])


def generic_rows(results):
    """the rows of bench/results/generic.tsv (steps/generic.py), [] when the file is not there"""
    gpath = os.path.join(results, 'generic.tsv')
    return extras.read(gpath)[1] if os.path.exists(gpath) else []


def engine_pairs(results, prefix, *arms):
    """[(case, engine / pg_sift)] of one generic suite: the engine's time (the faster of ARMS) is bench/results/generic.tsv (steps/generic.py, its own run and PostgreSQL build), pg_sift's that of cases.tsv"""
    cpath = os.path.join(results, 'cases.tsv')
    if not os.path.exists(cpath):
        return []
    sift = {f['case'].id: f['sift'] for f in cases_render.collect(extras.read(cpath)[1], cases_mod.read_registry()) if f['metric'] == 'ms' and f['sift']}
    other = {}
    for r in generic_rows(results):
        if r['arm'] in arms and r['metric'] == 'ms' and r['value'] not in ('NA', ''):
            other[r['case']] = min(float(r['value']), other.get(r['case'], float('inf')))
    return [(c, o / sift[c]) for c, o in other.items() if c.startswith(prefix) and c in sift]


ARM_LABEL = {'pg': 'PostgreSQL 18 (the pg_duckdb image, its own plan)', 'pg_duckdb': 'pg_duckdb', 'duckdb': 'DuckDB (command line client, in-memory tables)', 'clickhouse': 'ClickHouse', 'citus': 'Citus (columnar tables)',
             'citus_dist': 'Citus (heap tables distributed over 4 shards of the one node)', 'timescaledb': 'TimescaleDB (hypertable on the time column of the time series)', 'timescaledb_cagg': 'TimescaleDB (continuous aggregate, the cases that have one)',
             'elasticsearch': 'Elasticsearch (SQL endpoint)'}


def rate_pairs(S, tier, others, variant, metric, inverse=False):
    """throughput (pg_sift over the system) or time (the system over pg_sift) of a phase: the best layout of each side"""
    pick = max if not inverse else min
    mine = pick(v for e in ('sift', 'sift_maint', 'sift_table', 'sift_table_maint') if (v := S.v(tier, e, '-', variant, metric)))
    out = {}
    for label, es in others.items():
        vals = [v for e in es if (v := S.v(tier, e, '-', variant, metric))]
        if vals:
            theirs = pick(vals)
            out[label] = [(variant, mine / theirs if not inverse else theirs / mine)]
    return out


def rows_of(results):
    """[(class, {engine: cell text})] for the analytics matrix"""
    d = bench_charts.load(results)
    S, SS, D1, D10 = d['S'], d['SS'], d['D1'], d['D10']
    t1 = S.tier_of(1_000_000, 3)
    qs = [q for q in S.queries]
    est = ('pg', 'ch', 'es', 'citus', 'duckdb')
    rows = []

    def store_row(name, qq, st=S, D=D1, tier=t1):
        rows.append((name, {e: cell(store_pairs(st, D, tier, e, qq)) for e in est}))

    store_row('Point lookup by key', ['01'])
    store_row('Counts and sums by group, distinct counts, time buckets', ['02', '03', '04', '06'])
    store_row('Filtered average, range count', ['05', '07'])
    store_row('Text search with filters: a count, a group-by, the newest 10 (pg_duckdb: a substring match)', ['08', '09', '10'])
    if SS:
        t10 = SS.tier_of(10_000_000, 3)
        if t10:
            store_row('The 7 questions at 10,000,000 orders (400x the rows of the smallest size)', qs, SS, D10, t10)
    whole, sets = shape_pairs(results, 'whole'), shape_pairs(results, 'set')
    if whole.get('pg'):
        rows.append(('Application shapes, whole table (8 reports: counts, groups, distinct, percentiles)', {'pg': cell(whole['pg']), 'ch': cell(whole['ch']), 'es': NOT, 'citus': NOT, 'duckdb': NOT}))
    if sets.get('pg'):
        rows.append(('Application shapes, reports of a set of tenants (7 shapes at 10, 400 and 4,000 tenants)', {'pg': cell(sets['pg']), 'ch': cell(sets['ch']), 'es': NOT, 'citus': NOT, 'duckdb': NOT}))
    cp = case_pairs(results)
    if cp:
        pairs, kept = cp
        rows.append((f'Benchmark cases ({len(pairs) + kept} query shapes; the planner kept PostgreSQL\'s plan on {kept})', {'pg': cell(pairs), 'ch': NOT, 'es': NOT, 'citus': NOT, 'duckdb': NOT}))
    for name, prefix in GENERIC:
        gp = case_pairs(results, prefix)
        if gp and (gp[0] or gp[1]):
            pairs, kept = gp
            rows.append((name, {'pg': cell(pairs) + (f"; the planner kept PostgreSQL's plan on {kept}" if kept else ''), 'ch': cell(engine_pairs(results, prefix, 'clickhouse')),
                                'es': cell(engine_pairs(results, prefix, 'elasticsearch')), 'citus': cell(engine_pairs(results, prefix, 'citus', 'citus_dist')), 'duckdb': cell(engine_pairs(results, prefix, 'pg_duckdb', 'duckdb'))}))
    tier = t1
    others = {'pg': ['pg'], 'ch': ['ch', 'ch_mut', 'ch_rmt'], 'es': ['es'], 'citus': ['citus']}
    for name, variant, metric, inv in (('Update 10% of the rows, rows per second (ClickHouse: its fastest way)', 'update 10%', 'phase_rows_s', False),
                                       ('Delete 5% of the rows, rows per second', 'delete 5%', 'phase_rows_s', False),
                                       ('Load of the 1,000,000 rows (data, indexes, merge)', '-', 'load_s', True)):
        r = rate_pairs(S, tier, others, variant, metric, inv)
        row = {e: single(r[e][0][1], 'faster', 'slower') if e in r else NOT for e in est}
        if metric == 'phase_rows_s':
            row['citus'] = 'n/a: columnar tables are append-only'
        rows.append((name, row))
    return rows


def size_row(S, tier):
    """the disk of each system against the smallest pg_sift layout: a ratio of sizes, below 1 pg_sift is larger"""
    mine = min(v for e in MINE if (v := store_charts.sizes(S, tier, e)[2]))
    out = {}
    for e in ('pg', 'ch', 'es', 'citus'):
        s = store_charts.sizes(S, tier, e)[2] if e in S.engines(tier) else None
        out[e] = single(s / mine, 'smaller', 'larger') if s else NOT
    return ('Size on disk, table and indexes (against `sift_table`, the smaller layout; `sift_col` keeps the heap)', {**out, 'duckdb': NOT})


def sources(results):
    S = store_charts.Store(os.path.join(results, 'store.json'))
    t1 = S.tier_of(1_000_000, 3)
    m = S.tiers[t1]
    sv, h = m['servers'], S.meta['host']
    date = m['parts']['warm']['date'][:10]
    an = bench_charts.json.load(open(os.path.join(results, 'analytics.json')))['meta'] if os.path.exists(os.path.join(results, 'analytics.json')) else {}
    duck_meta = {}
    p = os.path.join(results, 'duckdb.tsv')
    if os.path.exists(p):
        duck_meta = store_charts.read_tsv(p)[0]
    host = f'{h["cpu"].replace(" 8-Core Processor", "")}, {h["threads"]} threads, {h["ram"]} RAM'
    lines = [
        '| system | version | data | host and settings | warm or cold | date | results |',
        '|---|---|---|---|---|---|---|',
        f'| PostgreSQL, pg_sift | {sv["pg"].split()[0]}, pg_sift {sv["sift_sift"]} | 1,000,000 and 10,000,000 orders | {host}; a docker container of {m["cpus"]} CPUs (a quota) and {h["memory"]}; {sv["pg_settings"]} | warm: median of {m["runs"]} runs; cold: first run after the memory was emptied | {date} | [store.json](bench/results/store.json), [store-scaling.json](bench/results/store-scaling.json) |',
        f'| Elasticsearch | {sv["es"]} (Lucene {sv["es_lucene"]}) | the same rows | the same container limits; {m["cpus"]} shards, heap {sv["es_heap"].replace("-Xms4g -Xmx4g", "4 GB")}, shard request cache off | same | {date} | same |',
        f'| ClickHouse | {sv["ch"]} | the same rows | the same container limits; `max_threads` {sv["ch_threads"]}; MergeTree ordered by country and date | same | {date} | same |',
        f'| Citus columnar | {sv["citus_columnar"]} (PostgreSQL {sv["citus"].split()[0]}) | the same rows | the same container limits; one node, default columnar settings | same | {date} | same |',
    ]
    if duck_meta:
        d = duck_meta
        lines.append(f'| pg_duckdb | {d.get("pg_duckdb", "?").split(",")[0]} (DuckDB {d.get("duckdb", "?").split(" ")[1] if " " in d.get("duckdb", "") else "?"}) | 1,000,000 and 10,000,000 orders, the same CSV as the others | its own run: {d.get("cpu", "?").replace(" 8-Core Processor", "")}; a different PostgreSQL build ({(re.search(r"(PostgreSQL [\d.]+)[^,]*with pg_duckdb", d.get("postgresql", "")) or [None, "?"])[1]}); `psql` client; the better of its heap and Parquet modes | warm: median of 3; cold: first run in a new session | {d.get("date", "?")} | [duckdb.tsv](bench/results/duckdb.tsv), [duckdb-10000000.tsv](bench/results/duckdb-10000000.tsv) |')
    if an:
        lines.append(f'| PostgreSQL heap and flat tables, ClickHouse (the application shapes) | PostgreSQL {sv["pg"].split()[0]}, pg_sift {sv["sift_sift"]}; ClickHouse: a throwaway server, version not recorded in the results | {an.get("tier", "?")} orders of an analytics corpus | one backend, no parallel workers; ClickHouse one thread | warm: median of {an.get("runs", "?")} runs | {an.get("date", "?")[:10]} | [analytics.json](bench/results/analytics.json) |')
    return lines


def search_tables(results):
    """the articles classes against every engine (pg_sift's queries per second over the engine's), and the SQLite and MariaDB ports against FTS5 and InnoDB FULLTEXT"""
    tt = readme.Tier(readme.load(results), 'slim')
    if not tt.articles:
        return []
    engines = [e for e in tt.articles if e not in ('sift', 'sift_nopos')]
    n = f'{tt.corpus("articles"):,}'
    head = ['| pg_sift\'s queries per second over | ' + ' | '.join(f'`{c}`' for c in tt.classes) + ' | index MiB |', '|---|' + '---:|' * (len(tt.classes) + 1)]
    mine_mib = tt.num('articles', 'sift', metric='index_mib')
    body, slower = [], {}
    for e in engines:
        cells = []
        for c in tt.classes:
            r = readme.ratio(tt.qps('sift', c), tt.qps(e, c))[0]
            cells.append(x(r) if r else 'n/a')
            if r and r <= 1 - TIE:
                slower.setdefault(tt.label(e), []).append(f'`{c}` ({x(1 / r)})')
        mib = tt.num('articles', e, metric='index_mib')
        body.append(f'| {tt.label(e)} | ' + ' | '.join(cells) + f' | {mib:.0f} |' if mib else f'| {tt.label(e)} | ' + ' | '.join(cells) + ' | n/a |')
    body.append(f'| pg_sift (the row compared) | ' + ' | '.join(f'{tt.qps("sift", c)["value"] if isinstance(tt.qps("sift", c), dict) else readme.number(tt.qps("sift", c)):,.0f} qps' for c in tt.classes) + f' | {mine_mib:.0f} |')
    s = tt.session
    out = [f'{n} Simple English Wikipedia articles, 8 clients, every engine on the same data and queries ({s.get("quota", "3 CPUs")} for each server, {s.get("cpu", "?")}, {s.get("date", "?")}, PostgreSQL 18.6, Elasticsearch {s.get("elasticsearch", "?")}); '
           'the ratio is pg_sift\'s throughput over the engine\'s, so above 1x pg_sift is faster. Not in the table: PostgreSQL\'s own full-text search (the `gin` engine of `bench/engines.tsv` has no stored result) and OpenSearch (no adapter).', ''] + head + body
    if slower:
        out += ['', 'pg_sift is slower than ' + '; '.join(f'{e} on ' + ', '.join(cs) for e, cs in slower.items()) + '.']
    return out


def port_lines():
    """the ports against the embedded or in-server alternatives: the ratio column of the tables of sqlite/README.md and mariadb/README.md, with their date and the load of the host"""
    out = []
    for path, name, what, pat in (('sqlite/README.md', 'SQLite FTS5', 'ranked top-10 AND, OR and phrase', r'\|\s*(AND|OR|phrase) top 10\s*\|.*\|\s*(\d+)x\s*\|\s*$'),
                                  ('mariadb/README.md', 'MariaDB InnoDB FULLTEXT', 'ranked top-10 AND, OR and phrase', r'\|\s*(AND|OR|phrase) top-10\s*\|.*\|\s*([\d,]+)x(?: / ([\d,]+)x)?\s*\|\s*$')):
        p = os.path.join(REPO, path)
        if not os.path.exists(p):
            continue
        text = open(p).read()
        sec = text[text.index('## Numbers against'):]
        sec = sec[:sec.index('\n## ', 3)] if '\n## ' in sec[3:] else sec
        vals = []
        for line in sec.splitlines():
            m = re.match(pat, line)
            if m:
                vals += [int(g.replace(',', '')) for g in m.groups()[1:] if g]
        hist = re.search(r'\*\*Historical baseline \(([\d-]+), ([^;)]*?)(?:;[^)]*)?\)', sec)
        load = re.search(r'load average ([\d ]+(?:to \d+)?)', sec)
        size = re.search(r'\|[^|]*index, with positions[^|]*\|[^|]*\|[^|]*\|[^|]*\|\s*([\d.]+x (?:larger|smaller))\s*\|', sec) or re.search(r'Index size: .*?: ([\d.]+x smaller)', sec)
        if vals:
            out.append((name, what, f'{min(vals):,}x to {max(vals):,}x', hist.group(1) + ', ' + hist.group(2) if hist else '?', load.group(1) if load else '?',
                        size.group(1) if size else NOT, path))
    return out


def ports_table():
    ports = port_lines()
    if not ports:
        return []
    rows = ['| the port against | measured | pg_sift faster by | index size | when and on what | host load | source |', '|---|---|---:|---|---|---|---|']
    for name, what, rng_, when, load, size, path in ports:
        rows.append(f'| {name} | {what}, 218,058 articles | {rng_} | {size} | **historical baseline**, {when} | load average {load} | [{path}]({path}) |')
    return rows


def compare_block(results):
    S = store_charts.Store(os.path.join(results, 'store.json'))
    rows = rows_of(results)
    rows.append(size_row(S, S.tier_of(1_000_000, 3)))
    est = ('pg', 'ch', 'es', 'citus', 'duckdb')
    head = ['| workload | ' + ' | '.join(LABEL[e] for e in est) + ' |', '|---|' + '---|' * len(est)]
    body = [f'| {name} | ' + ' | '.join(cells.get(e, NOT) for e in est) + ' |' for name, cells in rows]
    return ['On how many questions of a workload pg_sift is faster than each system, and by how much: the faster of its two layouts against the system, warm medians, a tie is within 5%. '
            f'Losses are in bold; `{NOT}` is a comparison no result file holds. The conditions of every number (engine and version, data size, host, settings, date) are under the table.', ''] + head + body


def generic_block(results):
    """docs/BENCHMARKS.md, block generic: per engine and arm of bench/results/generic.tsv what is compared with pg_sift and what is not, with the reason of each case an engine did not run"""
    rows = generic_rows(results)
    if not rows:
        return ['Not measured at this build yet: `bench/bench generic --docs` writes bench/results/generic.tsv.']
    suites = {c.id: c.dataset for c in cases_mod.read_registry() if c.dataset in {'tpch', 'hits', 'tseries'}}
    head = ['| engine and arm | cases it ran | pg_sift against it | cases it did not run, and why |', '|---|---:|---|---|']
    body = []
    for arm in dict.fromkeys(r['arm'] for r in rows):
        mine = [r for r in rows if r['arm'] == arm]
        ran = [r for r in mine if r['value'] not in ('NA', '')]
        why = {}
        for r in mine:
            if r['value'] in ('NA', ''):
                why.setdefault(r['note'] or 'no reason recorded', []).append(r['case'])
        pairs = [p for _, prefix in GENERIC for p in engine_pairs(results, prefix, arm)] if arm != 'pg' else []
        body.append('| %s | %d of %d | %s | %s |' % (ARM_LABEL.get(arm, arm), len(ran), len(mine), cell(pairs) if pairs else 'the reference (see the cases)' if arm == 'pg' else NOT,
                                                   '; '.join('%s (%s)' % (reason.replace('|', '\\|'), ', '.join('`%s`' % c for c in cs)) for reason, cs in why.items()) or '-'))
    return ['The 19 queries of the generic suites (TPC-H-like, ClickBench-like, time series) on every engine of `bench/engines.tsv` that runs analytic queries, from bench/results/generic.tsv: one engine at a time, the answers compared with '
            'PostgreSQL\'s before any time is taken, the same statement (the case\'s `--@ pg`, or the section of the engine\'s dialect). A case an engine does not run is listed with the reason it gave; it is never a ratio against anything. '
            'pg_sift\'s number is that of bench/results/cases.tsv (the planner\'s own choice; a case where it keeps PostgreSQL\'s plan is in the gap table above).', ''] + head + body


def sources_block(results):
    return ['<details>', '<summary>Conditions of every number: engine, version, data, host, settings, warm or cold, date</summary>', ''] + sources(results) + ['', '</details>']


def search_block(results):
    return search_tables(results)


def ports_block(results):
    return ports_table()


def numbers_block(results):
    d = bench_charts.load(results)
    S, SS, D1, D10 = d['S'], d['SS'], d['D1'], d['D10']
    t1 = S.tier_of(1_000_000, 3)
    est = [e for e in bench_charts.ORDER]

    def table(st, D, tier, qs, size_label):
        es = [e for e in est if e in st.engines(tier) or e == 'duckdb' and D]
        head = ['| question | ' + ' | '.join(bench_charts.SHORT[e] + ('†' if e == 'duckdb' else '') for e in es) + ' |', '|---|' + '---:|' * len(es)]
        body = []
        for q in qs:
            vals = {e: bench_charts.lat(st, D, tier, e, q) for e in es}
            vals = {e: v[0] for e, v in vals.items() if v}
            lo = min(vals.values())
            cells = [(f'**{extras.ms(vals[e])}**' if vals[e] <= lo * (1 + TIE) else extras.ms(vals[e])) if e in vals else 'n/a' for e in es]
            body.append(f'| Q{q} {SLUG[q]} | ' + ' | '.join(cells) + ' |')
        return head + body

    qs = list(S.queries)
    out = [f'Median of the warm runs in milliseconds on 1,000,000 orders, the fastest of a row and anything within 5% of it in bold; pg_sift is `sift_col` or `sift_table` (the table itself in column format). '
           '† pg_duckdb is its own run: the better of its heap and Parquet modes, a different PostgreSQL build and a `psql` client, so its sub-millisecond cells differ from the others (conditions above).', '']
    out += table(S, D1, t1, qs, '1M')
    if SS and SS.tier_of(10_000_000, 3):
        t10 = SS.tier_of(10_000_000, 3)
        out += ['', 'The same questions on 10,000,000 orders (warm medians, ms):', ''] + table(SS, D10, t10, qs, '10M')
    return out


def render(results, readme_path):
    for name, fn in (('compare', compare_block), ('compare-sources', sources_block), ('compare-search', search_block), ('compare-ports', ports_block), ('numbers', numbers_block)):
        readme.replace_block(readme_path, name, fn(results))
    benchmarks = os.path.join(os.path.dirname(readme_path), 'docs', 'BENCHMARKS.md')
    if os.path.exists(benchmarks) and '<!-- bench:generic:begin' in open(benchmarks).read():
        readme.replace_block(benchmarks, 'generic', generic_block(results))


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--results', default=os.path.join(REPO, 'bench', 'results'))
    ap.add_argument('--readme', default=os.path.join(REPO, 'README.md'))
    a = ap.parse_args()
    render(a.results, a.readme)


if __name__ == '__main__':
    main()
