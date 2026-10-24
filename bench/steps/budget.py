#!/usr/bin/env python3
"""The time budget of the release pass (block `budget`): what each stage costs and where the seconds go, per engine and per case, from bench/presets.env and the
committed results (bench/results/store.json, articles.tsv, places.tsv, cases.tsv, cases-time.tsv), standard library only. Nothing here measures; a number that no result file holds is
`not measured`.

    python3 -I bench/steps/budget.py [--results DIR] [--benchmarks FILE]      (steps/highlights.py runs it)
"""
import argparse
import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import extras  # noqa: E402
import readme  # noqa: E402  (replace_block)
import store_charts  # noqa: E402

REPO = os.path.dirname(os.path.dirname(HERE))
NOT = 'not measured'
STAGES = [  # stage, what it times, the engines
    ('store', 'the seven aggregate and three search questions at 1 and 3 CPUs, warm and cold, update rates', 'PostgreSQL, pg_sift (2 layouts, 2 with maintenance), Citus columnar, Elasticsearch, ClickHouse (3 update paths)'),
    ('updates', 'the update, delete and VACUUM section: bulk changes at 200,000 (3 runs) and 1,000,000 (2 runs) orders, steady churn, inserts only', 'PostgreSQL, pg_sift (sift_col, sift_table, with and without maintenance)'),
    ('compression', 'the compression variants of the column index', 'pg_sift, and the size of the same table in each system'),
    ('stats', 'what ClickHouse and Elasticsearch read per question', 'ClickHouse, Elasticsearch'),
    ('gap', 'the profile of pg_sift under a memory cap', 'pg_sift'),
    ('search', 'articles (8 clients) and places (1 client), warm and cold', 'pg_sift, Elasticsearch, pg_search, pg_textsearch, rum, pgx-bm25'),
    ('cases', 'every case of bench/cases.tsv at 1,000,000 orders (tables shared by the cases that read the same one)', 'pg_sift, PostgreSQL (`sift.enable_agg = off`), pg_duckdb'),
    ('duckdb', 'the ten questions of the store at 1,000,000 orders', 'pg_duckdb (heap and Parquet), pg_sift, PostgreSQL'),
]
OUTSIDE = [  # what the pass does not run: a release runs it by name when its code changed
    ('scale', 'the slopes of the cases of the README table (bench/results/cases-scale.tsv)', '`bench cases --cases ID,ID,ID --rows 100000 --scales 1,3,10 --docs` for the cases that changed, about 5 minutes for 3 cases (measured); the stage `scale` (`--scales 1,10,100` of 200,000 orders, 20 million at the top) is hours and stays opt-in'),
    ('store sizes', 'the store questions at 25,000, 100,000 and 10,000,000 orders (bench/results/store-scaling.json)', '`bench store all --sizes 25000,100000,10000000 --cold-runs 0 --restart-runs 0 --update-sizes none`; about 16 minutes (10M: generation and the loads of Elasticsearch, Citus and PostgreSQL dominate), estimated from the loads of the 1M pass, not measured'),
    ('duckdb 10M', 'the ten questions of the store on pg_duckdb at 10,000,000 orders', '`bench duckdb --rows 10000000 --out bench/results/duckdb-10000000.tsv`; about 5 minutes, estimated, not measured'),
    ('sets, extras', 'the set-filter benchmark (25 min) and the release-to-release items (100 min: tags are rebuilt)', 'docs/BENCHMARKS.md only; `bench final --stages sets,extras`'),
]


def presets():
    """P_ variables of the standard preset, by sourcing bench/presets.env"""
    out = subprocess.run(['bash', '-c', '. bench/presets.env; preset standard; set | grep "^P_"'], cwd=REPO, capture_output=True, text=True).stdout
    return dict(l.split('=', 1) for l in out.splitlines() if '=' in l)


def minutes(p, stage):
    store = sum(int(p.get('P_MIN_' + k, 0)) for k in ('SETUP', 'WARM', 'COLD', 'UPDATES'))
    return {'store': store, 'updates': int(p.get('P_MIN_SECTION', 0)), 'compression': int(p.get('P_MIN_COMPRESSION', 0)), 'stats': int(p.get('P_MIN_STATS', 0)), 'gap': int(p.get('P_MIN_GAP', 0)),
            'search': int(p.get('P_MIN_SEARCH', 0)), 'cases': int(p.get('P_MIN_CASES', 0)), 'duckdb': int(p.get('P_MIN_DUCKDB', 0))}[stage]


def stage_table(p):
    total = sum(minutes(p, s) for s, _, _ in STAGES)
    out = ['| stage | minutes | what it times | engines |', '|---|---:|---|---|']
    out += ['| `%s` | %d | %s | %s |' % (s, minutes(p, s), what, eng) for s, what, eng in STAGES]
    out += ['| **release pass** | **%d** | `bench final --size standard --docs --remeasure-all`, one stage after another; the budget it checks is 120 minutes | every engine measured, none from the cache |' % total]
    return out, total


def engines_table(results):
    path = os.path.join(results, 'store.json')
    if not os.path.exists(path):
        return []
    S = store_charts.Store(path)
    tier = S.tier_of(1_000_000, 3)
    if not tier:
        return []
    lat = lambda e, q: (S.get(tier, e, q, 'warm', 'latency_ms') or {}).get('value')
    runs = int(S.tiers[tier].get('runs', 5)) + int(S.tiers[tier].get('warmups', 2))
    names = {'pg': 'PostgreSQL', 'sift': 'pg_sift `sift_col`', 'sift_table': 'pg_sift `sift_table`', 'es': 'Elasticsearch', 'ch': 'ClickHouse', 'citus': 'Citus columnar'}
    phase = lambda e: sum((S.get(tier, e, '-', v, 'phase_s') or {}).get('value', 0) for v in ('update 1%', 'update 10%', 'delete 5%'))
    out = ['| engine | load of 1,000,000 orders (s) | the ten questions, %d timed executions each (s) | update 1%%, update 10%%, delete 5%% (s) |' % runs, '|---|---:|---:|---:|']
    for e in ('pg', 'sift', 'sift_table', 'es', 'ch', 'citus'):
        q = [lat(e, x) for x in S.queries + list(store_charts.SEARCH)]
        warm = sum(v for v in q if v) * runs / 1000 if any(q) else None
        load = S.v(tier, e, metric='load_s')
        ph = phase(e) if e != 'citus' else None
        out.append('| %s | %s | %s | %s |' % (names[e], '%.1f' % load if load else NOT, '%.1f' % warm if warm is not None else NOT, '%.0f' % ph if ph else ('n/a: append-only' if e == 'citus' else NOT)))
    ph_ch = [(n, (S.get(tier, e, '-', 'delete 5%', 'phase_s') or {}).get('value')) for n, e in (('ClickHouse delete 5%, the lightweight DELETE', 'ch'),)]
    slow = [f'{n}: {v:.0f} s' for n, v in ph_ch if v and v > 60]
    return out + ([''] + ['The one cell above a minute: ' + '; '.join(slow) + ' (the mutation runs to its end; the same step takes 0.1 to 7 s on the others).'] if slow else [])


def search_table(results):
    rows = []
    for ds, name in (('articles', 'articles'), ('places', 'places')):
        p = os.path.join(results, ds + '.tsv')
        if not os.path.exists(p):
            return []
        for l in open(p):
            f = l.rstrip('\n').split('\t')
            if len(f) > 7 and f[1] == 'slim' and f[5] in ('wall_s', 'build_s') and f[0] == ds:
                rows.append((ds, f[2], f[4], f[5], float(f[6])))
    walls = {(d, v): s for d, e, v, m, s in rows if m == 'wall_s'}
    builds = {(d, e): s for d, e, v, m, s in rows if m == 'build_s'}
    out = ['| search | restore and build (s) | measure (s) | `CREATE INDEX` per engine (s) |', '|---|---:|---:|---|']
    for ds in ('articles', 'places'):
        b = ', '.join('%s %.1f' % (e, s) for (d, e), s in builds.items() if d == ds)
        out.append('| %s | %s | %s | %s |' % (ds, '%.0f' % walls[(ds, 'restore and build')] if (ds, 'restore and build') in walls else NOT, '%.0f' % walls[(ds, 'measure')] if (ds, 'measure') in walls else NOT, b or NOT))
    return out + ['', 'The cold pass of each dataset restores and builds the lanes again (articles 71 s, places 59 s) and measures 21 s and 46 s.']


def read_times(path):
    out = {}
    if os.path.exists(path):
        for l in open(path):
            f = l.rstrip('\n').split('\t')
            if not l.startswith('#') and f[0] != 'case' and len(f) == 4:
                out[f[0]] = (int(f[1]), float(f[2]), float(f[3]))
    return out


def cases_table(results):
    times = read_times(os.path.join(results, 'cases-time.tsv'))
    if not times:
        return ['The seconds of each case are not measured yet: `bench/bench cases --docs` writes bench/results/cases-time.tsv.']
    one = {k: v for k, v in times.items() if not k.startswith('(')}
    extra = {k: v for k, v in times.items() if k.startswith('(')}
    rows = sorted({v[0] for v in one.values()})
    n = max(rows, key=lambda r: sum(1 for v in one.values() if v[0] == r))
    one = {k: v for k, v in one.items() if v[0] == n}
    build, run = sum(v[1] for v in one.values()), sum(v[2] for v in one.values())
    fixed = sum(v[1] for v in extra.values())
    out = ['%d cases at %s orders on one backend: %.0f s of table builds (a case that reads the table of an earlier one has 0), %.0f s of timed runs (3 rounds of 1 untimed and 5 timed runs on every lane, a lane stops repeating after 20 s), '
           '%.0f s to generate the orders and load the pg_duckdb container: **%.0f minutes** (without the container\'s own runs).' % (len(one), format(n, ','), build, run, fixed, (build + run + fixed) / 60), '',
           '<details><summary>Seconds of each case: build and timed runs</summary>', '', '| case | build (s) | timed runs (s) |', '|---|---:|---:|']
    out += ['| `%s` | %.1f | %.1f |' % (k, v[1], v[2]) for k, v in sorted(one.items(), key=lambda kv: -(kv[1][1] + kv[1][2]))]
    return out + ['', '</details>']


def block(results):
    p = presets()
    st, total = stage_table(p)
    out = ['Minutes of a release pass on the reference host (`bench/bench final --size standard`; the stages of `bench/presets.env`): the stage times are the stored times of the '
           'pass of 2026-10-22 (a test host) with the cuts of the preset applied by arithmetic (the cases stage: measured, below), to be replaced by the first pass on the release host. The pass fits %d of the 120 minutes; with the two 10M items below (21 minutes, estimated) it is %d, before the image build and the data of `bench store prepare` (about 10). The tables under it are what a run costs by engine and by case.' % (total, total + 21), ''] + st
    out += ['', 'What stays out of the pass, and what it costs when a release needs it:', '', '| what | covers | command and time |', '|---|---|---|']
    out += ['| %s | %s | %s |' % o for o in OUTSIDE]
    e = engines_table(results)
    if e:
        out += ['', 'By engine, from the store pass of bench/results/store.json (1,000,000 orders, 3 CPUs):', ''] + e
    s = search_table(results)
    if s:
        out += ['', 'Search (slim tier, bench/results/articles.tsv and places.tsv):', ''] + s
    out += ['', 'Cases:', ''] + cases_table(results)
    return out


def render(results, benchmarks):
    if os.path.exists(benchmarks) and '<!-- bench:budget:begin' in open(benchmarks).read():
        readme.replace_block(benchmarks, 'budget', block(results))


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--results', default=os.path.join(REPO, 'bench', 'results'))
    ap.add_argument('--benchmarks', default=os.path.join(REPO, 'docs', 'BENCHMARKS.md'))
    a = ap.parse_args()
    render(a.results, a.benchmarks)


if __name__ == '__main__':
    main()
