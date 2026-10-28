#!/usr/bin/env python3
"""The introduction of README.md (a few paragraphs of the strongest numbers with their conditions), from the committed results (bench/results), standard library only.

    python3 -I bench/steps/highlights.py [--results DIR] [--readme FILE]

Rewrites <!-- bench:intro:begin --> ... <!-- bench:intro:end -->: one paragraph each on the aggregates (against every system, with the growth of the median question), on search and the cost of a row,
on the benchmark cases, and on where pg_sift does not win; the machine and the method are the footnote [^bench]. Every number is computed here from store.json, duckdb.tsv, articles.tsv,
store-scaling.json, cases.tsv and cases-wm4g.tsv; a paragraph whose results file does not exist is left out, never typed by hand.
"""
import argparse
import os
import statistics
import sys
import textwrap

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import bench_charts  # noqa: E402  (steps/bench_charts.py: the scale points and slopes)
import cases as cases_mod  # noqa: E402  (steps/cases.py: the registry)
import budget  # noqa: E402  (steps/budget.py: the time budget of the release pass)
import compare  # noqa: E402  (steps/compare.py: the comparison blocks)
import limits  # noqa: E402  (steps/limits.py: the limitations and trade-offs)
import cases_render  # noqa: E402  (steps/cases_render.py: collect)
import extras  # noqa: E402  (steps/extras.py: read)
import readme  # noqa: E402  (steps/readme.py: the search tier)
import store_charts  # noqa: E402  (steps/store_charts.py: Store, read_tsv)

REPO = os.path.dirname(os.path.dirname(HERE))
URL = 'https://github.com/brauliobo/pg_sift/blob/main/'
TIE = 0.05
MINE = ('sift', 'sift_table')


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
        out += ', slower on ' + ', '.join(f'{q} ({x(r)})' for q, r in slower)
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
    cases = list(dict.fromkeys(r['case'] for r in rows if r['case'] != 'load' and r['case'].split(' ')[0] not in store_charts.SEARCH))
    pairs = []
    for c in cases:
        duck = [v for v in (get(c, 'duckdb_heap'), get(c, 'duckdb_parquet')) if v]
        if duck and get(c, 'sift'):
            pairs.append((c.split(' ')[0], min(duck) / get(c, 'sift')))
    return bullet(verdict(pairs, 'pg_duckdb %s (its own run, best of its modes)' % meta['pg_duckdb'].split(',')[0]), 2) if pairs else []


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


def behind(S, tier, variant):
    """the questions where neither layout of pg_sift is within 5% of the fastest other system: 'name (system, factor)'"""
    out = []
    for q in S.queries:
        others = {e: S.lat(tier, e, q, variant)['value'] for e in S.engines(tier) if e not in MINE and S.lat(tier, e, q, variant)}
        mine = best_mine(S, tier, q, variant)
        best = min(others, key=others.get)
        if mine and others[best] < mine * (1 - TIE):
            out.append(f'{q} by {store_charts.SYSTEMS[best][0]}, {x(mine / others[best])} faster')
    return out


def plain(bullets):
    """the words of a bullet list item as one sentence"""
    return ' '.join(l.strip().lstrip('- ').strip() for l in bullets)


def cases_summary(results):
    """one sentence on the cases of bench/results/cases.tsv (bench/results/cases-wm4g.tsv at a large work_mem): how many are faster than PostgreSQL on the same rows, by how much, and the ones that are not"""
    path = os.path.join(results, 'cases.tsv')
    if not os.path.exists(path):
        return ''
    meta, rows = extras.read(path)
    figs = [f for f in cases_render.collect(rows, cases_mod.read_registry()) if f['metric'] == f['case'].metrics[0] and f['metric'] == 'ms' and f['native'] and f['sift']]
    wins = [f['native'] / f['sift'] for f in figs if not f['kept'] and f['native'] / f['sift'] >= 1 + TIE]
    lose = [f for f in figs if not f['kept'] and f['native'] / f['sift'] <= 1 - TIE]
    kept = [f for f in figs if f['kept']]
    g = cases_render.by_gap(figs)
    gaps = 'with the scan forced: faster on %d, not faster on %d, cannot run on %d' % tuple(len(g.get(k, [])) for k in ('cost', 'declined', 'feature')) + (', not measured on %d' % len(g['unmeasured']) if g.get('unmeasured') else '')
    out = (f'Across {len(figs)} query shapes on {int(meta["orders"]):,} rows (reports with grouping sets and window functions, top-N, semi and anti joins, keyset pages, JSON arrays, lookups by key) pg_sift is faster than '
           f'PostgreSQL on the same table on {len(wins)} (median {x(statistics.median(wins))}, up to {x(max(wins))}); the planner kept PostgreSQL\'s plan on {len(kept)} (no ratio; {gaps}), and pg_sift is slower on {len(lose)}'
           + (' (' + ', '.join(f'{f["case"].id} {x(f["sift"] / f["native"])}' for f in lose) + ')' if lose else '') + '.')
    p4 = os.path.join(results, 'cases-wm4g.tsv')
    if os.path.exists(p4):
        m4, r4 = extras.read(p4)
        f4 = [f for f in cases_render.collect(r4, cases_mod.read_registry()) if f['metric'] == 'ms' and f['native'] and f['sift']]
        w4 = [f for f in f4 if not f['kept'] and f['native'] / f['sift'] >= 1 + TIE]
        out += f' At a large `work_mem` setting (`work_mem` = {m4.get("work_mem", "4GB")}) {len(w4)} of the {len(f4)} application-shaped cases measured there are faster (median {x(statistics.median(f["native"] / f["sift"] for f in w4))}).' if w4 else ''
    return out


def btree_loss(results):
    """the lookup by primary key, where the planner keeps the btree: its time against the scan's (the case point_agg_id, the variant `forced` with the btree plans off)"""
    path = os.path.join(results, 'cases.tsv')
    if not os.path.exists(path):
        return ''
    rows = extras.read(path)[1]
    g = lambda arm: extras.num(rows, case='point_agg_id', arm=arm, metric='ms')
    nat, sift, forced = g('native'), g('sift'), g('forced')
    if not (nat and sift):
        return ''
    return f'A btree still wins a lookup by key where the planner keeps it: {nat:.2f} ms for the primary key of {int(extras.read(path)[0]["orders"]):,} rows, {sift:.2f} ms with pg_sift' + (f' ({forced:.2f} ms when the scan is forced).' if forced else '.')


def block(results):
    """the introduction of the README: what is measured and the strongest numbers with their conditions, in a few paragraphs; the machine and the method are the footnote [^bench]"""
    S = store_charts.Store(os.path.join(results, 'store.json'))
    tier = S.tier_of(1_000_000, 3)
    t = S.tiers[tier]
    h = S.meta['host']
    v = versions(S, tier)
    qs = S.queries
    n = t['rows']
    rows = readme.load(results)
    tt = readme.Tier(rows, 'slim')
    sz = {e: store_charts.sizes(S, tier, e) for e in S.engines(tier)}
    load = {e: S.v(tier, e, metric='load_s') for e in S.engines(tier)}
    g = lambda e, vr, m, c='-': S.v(tier, e, c, vr, m)
    vs = []
    for e, label in (('pg', 'PostgreSQL'), ('ch', 'ClickHouse'), ('es', 'Elasticsearch'), ('citus', 'Citus columnar')):
        pairs = [S.lat(tier, e, q)['value'] / best_mine(S, tier, q) for q in qs if S.lat(tier, e, q)]
        vs.append(f'{label} {rng(pairs)}')
    duck = duckdb_line(results)
    if duck:   # 'pg_duckdb 1.1.1 (its own run, best of its modes): faster on 7 of 7 (6.5x to 253x)'
        text = plain(duck)
        vs.append('pg_duckdb ' + text[text.index('(', text.index('faster on')) + 1:].split(')')[0])
    d = bench_charts.load(results)
    best = lambda q, variant: min([S.lat(tier, e, q, variant)['value'] for e in S.engines(tier) if e not in MINE + ('es_cached',) and S.lat(tier, e, q, variant)] + [d['D1'].best(q, 'warm_ms')] * bool(d['D1'] and d['D1'].best(q, 'warm_ms')))
    front = sum(1 for q in qs if best_mine(S, tier, q) <= best(q, 'warm') * (1 + TIE))
    pgr = [S.lat(tier, 'pg', q)['value'] / best_mine(S, tier, q) for q in qs]
    first = (f'On {len(qs)} aggregate questions over {n:,} orders (a point lookup, counts and sums by group, distinct counts, a filtered average) the faster pg_sift layout is the fastest system, or within 5% of it, on {front} of {len(qs)} warm[^bench]: '
             f'{rng(pgr)} the speed of PostgreSQL, and against the others {", ".join(vs[1:])}. {scale_sentence(results)}')
    second = (plain(search(tt)).replace('Search on', 'Search on', 1) + ' ' if search(tt) else '') + (f'The column index adds {sz["sift"][1] / n * 1048576:.1f} bytes a row to the table (PostgreSQL\'s btrees and GIN: {sz["pg"][1] / n * 1048576:.1f}), '
              f'loads in {load["sift"]:.1f} s against PostgreSQL\'s {load["pg"]:.1f} s, and 10% of the rows update at {g("sift", "update 10%", "phase_rows_s"):,.0f} rows/s ({g("pg", "update 10%", "phase_rows_s"):,.0f} on PostgreSQL).')
    third = cases_summary(results)
    cold = behind(S, tier, 'cold')
    ch_size = sz['ch'][2]
    fourth = ('Where it does not win: ' + (f'cold (caches emptied) it is not the fastest on {len(cold)} of {len(qs)} questions: {"; ".join(cold)}. ' if cold else '')
              + f'ClickHouse loads faster ({load["ch"]:.1f} s) and tenants the table in less space ({ch_size:.0f} MiB against {sz["sift"][2]:.0f} MiB for sift_col, whose heap stays). ' + btree_loss(results))
    foot = (f'[^bench]: Measured {t["parts"]["warm"]["date"][:10]} on {h["cpu"]} ({h["threads"]} threads), PostgreSQL {v["pg"]}, {t["cpus"]} CPUs per server, one client, warm medians, every result checked against PostgreSQL\'s; '
            'the method, the other systems\' versions and every table are in [docs/BENCHMARKS.md].')
    return [first, '', second, '', third, '', fourth, '', foot]


def scale_sentence(results):
    """the growth of the median question from the smallest to the largest size, and its log-log slope, on sift_col and on PostgreSQL"""
    d = bench_charts.load(results)
    if not d['SS']:
        return ''
    sizes, pts = bench_charts.scale_points(d)
    g = bench_charts.growth_of(pts, sizes, bench_charts.questions(d['SS']))
    sl = lambda e: bench_charts.slope(sizes, g[e])
    return (f'From {bench_charts.rows_label(sizes[0])} to {bench_charts.rows_label(sizes[-1])} orders ({sizes[-1] // sizes[0]}x the rows) the median question takes {g["sift"][-1]:.1f}x as long on sift_col (log-log slope {sl("sift"):.2f}) and '
            f'{g["pg"][-1]:.0f}x on PostgreSQL ({sl("pg"):.2f}; 1.0 is time proportional to the rows).')


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--results', default=os.path.join(REPO, 'bench', 'results'))
    ap.add_argument('--readme', default=os.path.join(REPO, 'README.md'))
    a = ap.parse_args()
    b = block(a.results)
    readme.replace_block(a.readme, 'intro', b)
    compare.render(a.results, a.readme)
    limits.render(a.results, a.readme)
    budget.render(a.results, os.path.join(os.path.dirname(a.results), 'BENCHMARKS.md'))
    print('\n'.join(b))


if __name__ == '__main__':
    main()
