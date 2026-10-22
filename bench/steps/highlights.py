#!/usr/bin/env python3
"""The "Performance highlights" block of README.md, from the committed results (docs/results), standard library only.

    python3 -I bench/steps/highlights.py [--results DIR] [--readme FILE]      (scripts/gen-docs.py runs it)

Rewrites <!-- bench:highlights:begin --> ... <!-- bench:highlights:end -->. The text is meant to be copied into a plain-text e-mail: ASCII, bullets,
no HTML, no tables, full https URLs, lines of at most 76 columns, the host, the data and the PostgreSQL version said once. Every number is computed
here from store.json (the ten questions), duckdb.tsv, articles.tsv (search), store-scaling.json and scaling.tsv (scale); a line whose results file
does not exist is left out, never typed by hand.
"""
import argparse
import os
import statistics
import sys
import textwrap

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import aggregates_page  # noqa: E402  (steps/aggregates_page.py: the shapes)
import cases as cases_mod  # noqa: E402  (steps/cases.py: the registry)
import cases_render  # noqa: E402  (steps/cases_render.py: the headline cases)
import extras  # noqa: E402  (steps/extras.py: read)
import readme  # noqa: E402  (steps/readme.py: the search tier)
import store_charts  # noqa: E402  (steps/store_charts.py: Store, read_tsv, question_slugs)

REPO = os.path.dirname(os.path.dirname(HERE))
URL = 'https://github.com/brauliobo/pg_sift/blob/main/'
TIE = 0.05
MINE = ('sift', 'sift_table')
SLUG = store_charts.question_slugs()


def x(v):
    return f'{v:.0f}x' if v >= 9.5 else f'{v:.1f}x'


def rng(vals):
    lo, hi = min(vals), max(vals)
    return x(lo) if x(lo) == x(hi) else f'{x(lo)} to {x(hi)}'


def bullet(text, indent=0):
    pad = ' ' * indent
    return textwrap.wrap(text, 76, initial_indent=pad + '- ', subsequent_indent=pad + '  ')


def verdict(pairs, label):
    """pairs [(question, other / pg_sift)]: 'faster on 9 of 10 questions (1.5x to 231x), a tie on 1'"""
    n = len(pairs)
    faster = [r for q, r in pairs if r >= 1 + TIE]
    tie = [q for q, r in pairs if 1 - TIE < r < 1 + TIE]
    slower = [(q, 1 / r) for q, r in pairs if r <= 1 - TIE]
    out = f'{label}: faster on {len(faster)} of {n}' if faster else f'{label}: not faster on any of {n}'
    if faster:
        out += f' ({rng(faster)})'
    if tie:
        out += f', a tie on {len(tie)}'
    if slower:
        out += ', slower on ' + ', '.join(f'{SLUG[q]} ({x(r)})' for q, r in slower)
    return out


def best_mine(S, tier, q, variant='warm'):
    v = [S.lat(tier, e, q, variant)['value'] for e in MINE if S.lat(tier, e, q, variant)]
    return min(v) if v else None


def versions(S, tier):
    s = S.tiers[tier]['servers']
    major = lambda v: '.'.join(v.split()[0].split('.')[:2])
    return dict(pg=s['pg'].split()[0], es=major(s['es']), ch=major(s['ch']), citus=major(s['citus_columnar'].replace('-', '.')))


def duckdb_line(results):
    path = os.path.join(results, 'duckdb.tsv')
    if not os.path.exists(path):
        return []
    meta, rows = store_charts.read_tsv(path)
    get = lambda case, arm: next((float(r['value']) for r in rows if (r['case'], r['arm'], r['metric']) == (case, arm, 'warm_ms') and r['value'] not in ('NA', 'error', '')), None)
    cases = list(dict.fromkeys(r['case'] for r in rows if r['case'] != 'load'))
    pairs = []
    for c in cases:
        duck = [v for v in (get(c, 'duckdb_heap'), get(c, 'duckdb_parquet')) if v]
        if duck and get(c, 'sift'):
            pairs.append((c.split(' ')[0], min(duck) / get(c, 'sift')))
    return bullet(verdict(pairs, 'pg_duckdb %s (its own run, best of its modes)' % meta['pg_duckdb'].split(',')[0]), 2) if pairs else []


def aggregates(S, tier, results):
    v = versions(S, tier)
    qs = S.queries
    n = S.tiers[tier]['rows']
    out = bullet(f'Aggregates on {n:,} orders, {len(qs)} questions (count, sum, distinct, top-N, text search with filters); the faster of sift_col and sift_table against:')
    for e, label in (('pg', f'PostgreSQL {v["pg"]}'), ('ch', f'ClickHouse {v["ch"]}'), ('es', f'Elasticsearch {v["es"]}'), ('citus', f'Citus {v["citus"]} columnar')):
        pairs = [(q, S.lat(tier, e, q)['value'] / best_mine(S, tier, q)) for q in qs if S.lat(tier, e, q)]
        out += bullet(verdict(pairs, label), 2)
    out += duckdb_line(results)
    return out


def search(t):
    """the articles table: against pg_search and Elasticsearch, and the size"""
    r = {e: [readme.ratio(t.qps('sift', c), t.qps(e, c))[0] for c in t.classes] for e in t.articles if e not in ('sift', 'sift_nopos')}
    out = []
    n = f'{t.corpus("articles"):,}'
    if 'pg_search' in r and all(r['pg_search']):
        out += bullet(f'Search on {n} Wikipedia articles, 8 clients: {rng(r["pg_search"])} the queries per second of pg_search on every query class'
                      + (f'; ahead of Elasticsearch on {sum(1 for v in r["elasticsearch"] if v >= 1 + TIE)} of {len(r["elasticsearch"])} classes ({rng(r["elasticsearch"])} across all)' if 'elasticsearch' in r and all(r['elasticsearch']) else '')
                      + f'; the index is {t.num("articles", "sift", metric="index_mib"):.0f} MiB with phrase support ({readme.x(t.num("articles", "pg_search", metric="index_mib") / t.num("articles", "sift", metric="index_mib"))} smaller than pg_search).')
    return out


def sizes_and_load(S, tier):
    sz = {e: store_charts.sizes(S, tier, e) for e in S.engines(tier)}
    load = {e: S.v(tier, e, metric='load_s') for e in S.engines(tier)}
    n = S.tiers[tier]['rows']
    return (bullet(f'Size on {n:,} orders: sift_col adds {sz["sift"][1]:.1f} MiB to the table (PostgreSQL\'s btrees and GIN: {sz["pg"][1]:.1f} MiB); sift_table holds the table in {sz["sift_table"][2]:.0f} MiB '
                   f'(ClickHouse {sz["ch"][2]:.0f}, Citus {sz["citus"][2]:.0f}, Elasticsearch {sz["es"][2]:.0f}). Load from one CSV: sift_col {load["sift"]:.1f} s, PostgreSQL {load["pg"]:.1f} s, '
                   f'ClickHouse {load["ch"]:.1f} s, Elasticsearch {load["es"]:.0f} s.'))


def updates(S, tier):
    g = lambda e, v, m, c='-': S.v(tier, e, c, v, m)
    if not g('sift', 'update 10%', 'phase_rows_s'):
        return []
    q = '03'
    return bullet(f'Updates: 10% of the rows updated at {g("sift", "update 10%", "phase_rows_s"):,.0f} rows/s (PostgreSQL {g("pg", "update 10%", "phase_rows_s"):,.0f}); revenue per month '
                  f'under 10,000 updated rows/s: {g("sift", "load 10000/s", "latency_ms", q):.0f} ms on sift_col, {g("pg", "load 10000/s", "latency_ms", q):.0f} ms on PostgreSQL.')


def scale(results, S, tier):
    """rows and partitions and workers: only what the results files hold"""
    out = []
    path = os.path.join(results, 'store-scaling.json')
    if os.path.exists(path):
        A = store_charts.Store(os.path.join(results, 'store.json'), path)
        sizes_ = sorted({m['rows'] for m in A.tiers.values() if m.get('cpus') == 3})
        tiers = {n: A.tier_of(n, 3) for n in sizes_}
        lo, hi = tiers[sizes_[0]], tiers[sizes_[-1]]
        grow = lambda e: statistics.median(A.lat(hi, e, q)['value'] / A.lat(lo, e, q)['value'] for q in A.queries if A.lat(hi, e, q) and A.lat(lo, e, q))
        have = [q for q in A.queries if best_mine(A, hi, q) and any(A.lat(hi, e, q) for e in A.engines(hi) if e not in MINE)]
        wins = sum(1 for q in have if best_mine(A, hi, q) <= min(A.lat(hi, e, q)['value'] for e in A.engines(hi) if e not in MINE and A.lat(hi, e, q)) * (1 + TIE))
        out += bullet(f'Scale, {store_charts.rows_label(sizes_[0])} to {store_charts.rows_label(sizes_[-1])} orders ({sizes_[-1] // sizes_[0]:,}x the rows): the median question takes {grow("sift"):.0f}x as long on sift_col, '
                      f'{grow("pg"):.0f}x on PostgreSQL, {grow("ch"):.0f}x on ClickHouse; at {sizes_[-1]:,} orders pg_sift is fastest or within 5% on {wins} of {len(have)} questions.')
    path = os.path.join(results, 'scaling.tsv')
    if os.path.exists(path):
        meta, rows = store_charts.read_tsv(path)
        by = lambda case, arm: next((float(r['value']) for r in rows if (r['case'], r['arm'], r['metric']) == (case, arm, 'median_ms')), None)
        parts = sorted({int(r['case'].split()[1]) for r in rows if r['case'].startswith('partitions ')})
        qs = sorted({r['arm'].split('_')[0] for r in rows} - {'q01'})   # the lookup by id is not an aggregate
        if parts and qs:
            gain = lambda p: statistics.median(by(f'partitions {p}', f'{q}_pg') / by(f'partitions {p}', f'{q}_sift') for q in qs)
            g0, g1 = gain(parts[0]), gain(parts[-1])
            out += bullet(f'Partitions, {parts[0]} to {parts[-1]:,}: the speed-up over PostgreSQL on {len(qs)} aggregate questions (median) goes from {x(g0)} to {x(g1) if g1 >= 1 else x(1 / g1) + " slower"} ({parts[-1]:,} partitions).')
    return out


def shapes(results):
    found = aggregates_page.shape_cases(results)
    if not found:
        return []
    meta, cases = found
    gains = [(t['heap'] / t['col'], q, v) for q, v, t in cases if t['heap'] and t['col']]
    faster, slower = [g for g, q, v in gains if g >= 1 + TIE], [(1 / g, v) for g, q, v in gains if g <= 1 - TIE]
    out = f'Real query shapes (joins, lookups, top-N, distinct, time buckets, UNION ALL arms; {len(gains)} cases on {meta.get("tier", "?")} orders, one backend): faster than the PostgreSQL heap on {len(faster)} ({rng(faster)}, median {x(statistics.median(g for g, q, v in gains))})'
    return bullet(out + (f', slower on {len(slower)} ({rng([r for r, v in slower])}; {sum(1 for r, v in slower if v == "K10")} of them with the smallest filter set)' if slower else '') + '.')


def faster33(results):
    """the headline cases of bench/cases.tsv (docs/results/cases.tsv) against PostgreSQL on the same rows, and the cases that lose, said plainly"""
    path = os.path.join(results, 'cases.tsv')
    found = cases_render.headline(extras.read(path)[1], cases_mod.read_registry()) if os.path.exists(path) else None
    if not found:
        return [], []
    meta = extras.read(path)[0]
    parts, slower = found
    first = bullet(f'3.3 optimizations on {int(meta["orders"]):,} orders, one backend, against PostgreSQL on the same rows (sift.enable_agg = off): {parts}.')
    return first, bullet('3.3 cases that are slower than PostgreSQL: ' + ', '.join(f'{c} ({r})' for c, r in slower) + '.', 2) if slower else []


def behind(S, tier, variant):
    """the questions where neither layout of pg_sift is within 5% of the fastest other system: 'name (system, factor)'"""
    out = []
    for q in S.queries:
        others = {e: S.lat(tier, e, q, variant)['value'] for e in S.engines(tier) if e not in MINE and S.lat(tier, e, q, variant)}
        mine = best_mine(S, tier, q, variant)
        best = min(others, key=others.get)
        if mine and others[best] < mine * (1 - TIE):
            out.append(f'{SLUG[q]} ({store_charts.SYSTEMS[best][0]}, {x(mine / others[best])})')
    return out


def losses(S, tier):
    out = []
    qs = S.queries
    cold = behind(S, tier, 'cold')
    if cold:
        out += bullet(f'Cold (caches emptied), not the fastest on {len(cold)} of {len(qs)} questions: ' + ', '.join(cold) + '.', 2)
    t1 = S.tier_of(S.tiers[tier]['rows'], 1)
    one = behind(S, t1, 'warm') if t1 else []
    if one:
        out += bullet(f'With 1 CPU per server (warm), not the fastest on {len(one)} of {len(qs)} questions: ' + ', '.join(one) + '.', 2)
    cached = [(q, S.lat(tier, 'es_cached', q)['value'] / best_mine(S, tier, q)) for q in qs if S.lat(tier, 'es_cached', q)]
    ahead = [(q, 1 / r) for q, r in cached if r <= 1 - TIE]
    if ahead:
        out += bullet('Elasticsearch with its request cache on is ahead on ' + ', '.join(f'{SLUG[q]} ({x(r)})' for q, r in ahead) + ' (the cache is off in the other numbers).', 2)
    pg, sc, ch = (store_charts.sizes(S, tier, e)[2] for e in ('pg', 'sift', 'ch'))
    out += bullet(f'ClickHouse loads faster ({S.v(tier, "ch", metric="load_s"):.1f} s) and its table is smaller ({ch:.0f} MiB against {sc:.0f} MiB for sift_col, whose heap stays).', 2)
    return out


def updates_loss(S, tier):
    rate = lambda e: S.v(tier, e, '-', 'delete 5%', 'phase_rows_s')
    if rate('pg') and rate('sift') and rate('pg') > rate('sift') * (1 + TIE):
        return bullet(f'Deleting 5% of the rows: PostgreSQL {rate("pg"):,.0f} rows/s against sift_col {rate("sift"):,.0f}.', 2)
    return []


def block(results):
    S = store_charts.Store(os.path.join(results, 'store.json'))
    tier = S.tier_of(1_000_000, 3)
    t = S.tiers[tier]
    h = S.meta['host']
    cpu = h['cpu'].replace(' 8-Core Processor', '')
    rows = readme.load(results)
    tt = readme.Tier(rows, 'slim')
    out = textwrap.wrap(f'BM25 search and column-store aggregates for PostgreSQL 17 and 18 in one extension. Measured {t["parts"]["warm"]["date"][:10]} on {cpu} ({h["threads"]} threads), PostgreSQL {versions(S, tier)["pg"]}, {t["cpus"]} CPUs per server, one client, warm medians; every result is checked against PostgreSQL\'s.', 76) + ['']
    fast, slow = faster33(results)
    out += aggregates(S, tier, results) + search(tt) + sizes_and_load(S, tier) + updates(S, tier) + scale(results, S, tier) + shapes(results) + fast
    out += bullet('Where it does not win:') + losses(S, tier) + updates_loss(S, tier) + slow
    out += ['', 'Results, charts and method:', URL + 'docs/perf/AGGREGATES.md']
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--results', default=os.path.join(REPO, 'docs', 'results'))
    ap.add_argument('--readme', default=os.path.join(REPO, 'README.md'))
    a = ap.parse_args()
    b = block(a.results)
    assert max(len(l) for l in b) <= 78, max(b, key=len)
    readme.replace_block(a.readme, 'highlights', b)
    print('\n'.join(b))


if __name__ == '__main__':
    main()
