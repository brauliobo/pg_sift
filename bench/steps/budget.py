#!/usr/bin/env python3
"""The time budget of the release pass (block `budget`): what each stage costs and where the seconds go, per engine and per case, from bench/presets.env and the
committed results (bench/results/store.json, articles.tsv, places.tsv, cases.tsv, cases-time.tsv), standard library only. Nothing here measures; a number that no result file holds is
`not measured`.

    python3 -I bench/steps/budget.py [--results DIR] [--benchmarks FILE]      (steps/highlights.py runs it)
"""
import argparse
import math
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
    ('generic', 'the 35 queries of the generic suites (TPC-H-like, ClickBench-like, time series, and the 16 headline cases on the orders) at 1,000,000 rows, answers checked against PostgreSQL\'s, one engine at a time', 'pg_duckdb, DuckDB, ClickHouse, Citus (columnar, distributed), TimescaleDB (hypertable, continuous aggregate), Elasticsearch (pg_sift and PostgreSQL are in `cases`)'),
]
BASIS = {  # where the minutes of a stage come from
    'store': 'arithmetic on the stored parts of 2026-10-22 (load 32 s, warm 37 s, cold 602 s at 3 CPUs), with the preset\'s cold runs, restarts and load windows',
    'updates': 'arithmetic on the stored section of 2026-10-22 (720 s of update phases, 336 s of them ClickHouse\'s DELETE), with the preset\'s repetitions and churn windows',
    'compression': 'estimate', 'stats': 'estimate', 'gap': 'estimate',
    'search': 'arithmetic on the stored runs of 2026-10-22: articles 70 + 207 s, places 78 + 267 s, the cold passes 71 + 21 s and 59 + 46 s',
    'cases': 'estimate for the 200 cases of 2026-10-28 (`bench cases --plan`: 41 min); 91 cases were measured on 2026-10-24 (see Cases below), and the IO runs of every lane add about 0.4 s a case at 1,000,000 orders; the pg_duckdb lane of the cases is not in the pass, the `duckdb` and `generic` stages cover that engine',
    'duckdb': 'estimate from the load and the ten questions of the stored 1M run',
    'generic': 'estimate: pg_duckdb 4 (three generated datasets, CSV export, two lanes), Elasticsearch 3, Citus 2.5, TimescaleDB 1.5, ClickHouse 1, DuckDB 0.5, in all 14 (the orders suite adds about 2: one more table to generate, export and load in every engine, 16 more queries); the loaders and the answers are checked at 20,000 rows (32 s for the six engines), no timing taken',
}
SIFT_BUILD = 3  # minutes: the one step of the preparation that depends on the commit under test (the extension built into the image)
OUTSIDE = [  # what the pass does not run: a release runs it by name when its code changed
    ('scale', 'the slopes of the cases of the README table (bench/results/cases-scale.tsv)', '`bench cases --cases ID,ID,ID --rows 100000 --scales 1,3,10 --docs` for the cases that changed, about 5 minutes for 3 cases (measured); the stage `scale` (`--scales 1,10,100` of 200,000 orders, 20 million at the top) is hours and stays opt-in'),
    ('sets, extras', 'the set-filter benchmark (25 min) and the release-to-release items (100 min: tags are rebuilt)', 'docs/BENCHMARKS.md only; `bench final --stages sets,extras`'),
]


def sizes_seconds(results):
    """tier -> (load, warm) wall seconds of the store questions at each size, from the parts of bench/results/store-scaling.json"""
    path = os.path.join(results, 'store-scaling.json')
    if not os.path.exists(path):
        return {}
    S = store_charts.Store(path)
    wall = lambda tier, part: tier['parts'].get(part, {}).get('utilization', {}).get('wall_s', 0)
    return {t: (wall(tier, 'load'), wall(tier, 'warm')) for t, tier in S.tiers.items()}


def window_table(results, total):
    """the items that the window holds after the stages of the pass: the 10M runs and the build of the extension"""
    sz = sizes_seconds(results)
    small = sum(sum(v) for t, v in sz.items() if not t.startswith('10M')) / 60
    big = sum(sum(v) for t, v in sz.items() if t.startswith('10M')) / 60
    bl, bw = next((v for t, v in sz.items() if t.startswith('10M')), (0, 0))
    rows = [
        ('store at 25,000 and 100,000 orders', '`bench store all --sizes 25000,100000 --cold-runs 0 --restart-runs 0 --update-sizes none`', small, 'measured on 2026-10-22' if sz else NOT),
        ('store at 10,000,000 orders', '`bench store all --sizes 10000000 --runs 3 --warmups 1 --cold-runs 0 --restart-runs 0 --update-sizes none`', (bl + bw * 4 / 7) / 60,
         'arithmetic on the run of 2026-10-22 (load %.0f s, the ten questions %.0f s with 5 timed runs and 2 warm-ups, five of them measured again on a noisy disk): 3 timed runs and 1 warm-up scale the questions by 4/7' % (bl, bw) if sz else NOT),
        ('pg_duckdb at 10,000,000 orders', '`bench duckdb --rows 10000000 --out bench/results/duckdb-10000000.tsv`', 5, 'estimate'),
        ('the extension built into the image', 'the one step of the preparation that depends on the commit', SIFT_BUILD, 'estimate'),
    ]
    out = ['| in the window, after the stages | command | minutes | basis |', '|---|---|---:|---|']
    out += ['| %s | %s | %.0f | %s |' % r for r in rows]
    full = total + sum(r[2] for r in rows)
    out += ['| **release window** | the stages above, then these | **%.0f** | the stages are %d |' % (full, total)]
    return out, full


def presets(name='standard'):
    """P_ variables of a preset, by sourcing bench/presets.env"""
    out = subprocess.run(['bash', '-c', f'. bench/presets.env; ALL_UPDATE_ENGINES=x; preset {name}; set | grep "^P_"'], cwd=REPO, capture_output=True, text=True).stdout
    return dict(l.split('=', 1) for l in out.splitlines() if '=' in l)


def minutes(p, stage):
    store = sum(int(p.get('P_MIN_' + k, 0)) for k in ('SETUP', 'WARM', 'COLD', 'UPDATES'))
    return {'store': store, 'updates': int(p.get('P_MIN_SECTION', 0)), 'compression': int(p.get('P_MIN_COMPRESSION', 0)), 'stats': int(p.get('P_MIN_STATS', 0)), 'gap': int(p.get('P_MIN_GAP', 0)),
            'search': int(p.get('P_MIN_SEARCH', 0)), 'cases': int(p.get('P_MIN_CASES', 0)), 'duckdb': int(p.get('P_MIN_DUCKDB', 0)), 'generic': int(p.get('P_MIN_GENERIC', 0))}[stage]


RELEASE_WHY = {  # stage: what the release run cuts against the standard pass, and what keeps the published number
    'store': 'the parameters of the standard pass on purpose: the key of a cached cell holds the runs, warm-ups, cold runs, thread budgets and rates, so the cells of Elasticsearch, ClickHouse and Citus that the last pass on this host measured are read from the cache (date and key in the table); the whole cost when the cache is empty, the cached figure when it is not',
    'updates': 'one run at 200,000 and at 1,000,000 orders (standard: 3 and 2) and 5 s churn windows (10 s): the stored repetitions differ by 1.05x at the median cell (1.25x or less in 77% of 152 cells), so the table prints one run instead of the median of a few',
    'compression': 'unchanged: one build of each variant at 1,000,000 orders, sizes and build times, no timed runs',
    'stats': 'unchanged: counters of two servers, a minute', 'gap': 'unchanged: one profile under a memory cap, a minute',
    'search': 'two interleaved rounds for every cell (standard: 2 to 3; a cell above the error target keeps its flag instead of a third round); the engines and classes are all there, and a second run of `fuzzy` for pg_sift and Elasticsearch merged into the tier (the README cell that has no column yet: a restore and build of two engines and two interleaved rounds, about 2 minutes, under 1 GB of scratch: the 50,000-article slim tier, 72 MiB of Elasticsearch index and 30 MiB of pg_sift)',
    'cases': 'one round of 3 timed runs per lane (standard: 3 rounds of 5) and a lane budget of 4 s (10 s); NOT smaller: at 300,000 orders the median speed-up of the stored cases is 0.74x of the 1,000,000 one (only 9 of 26 cases within 25%), so the table stays at 1,000,000 orders, whose 15 minutes of table builds are the floor',
    'duckdb': 'unchanged: the ten questions, 3 warm runs, both pg_duckdb lanes',
    'generic': 'the other engines on three generated datasets, 3 runs, the answers checked first; this stage does not run pg_sift (its numbers are the cases stage\'s), so when its inputs key (harness, adapters, generators, statements, rows, runs, the CPU, threads and memory of the host) is the one in bench/results/generic.tsv the committed numbers stand in and the plan says `carried`: 0 minutes; the first pass of a host measures them',
    'scaling': 'partitions 1, 16, 256, 4096 and workers 0, 1, 3 (standard sweeps: 7 and 4 points), 3 runs per cell: the endpoints and the middle of each curve; the chart keeps its slope',
    'analytics': 'the 29 application shapes on 1,200,000 orders, 3 timed runs after one warm-up (5): heap, pg_sift, flat tables and ClickHouse',
}
RELEASE_STAGES = STAGES + [('scaling', 'the partition (1 to 4,096) and parallel worker (0 to 3) sweeps of four questions', 'PostgreSQL, pg_sift'),
                           ('analytics', 'the application shapes: whole-table reports and reports of a set of tenants', 'PostgreSQL heap, pg_sift, PostgreSQL flat tables, ClickHouse')]


def release_table():
    """the release run (steps/release.sh): the minutes of every stage from the preset `release`, against the budget"""
    p = presets('release')
    mins = lambda s: int({'store': sum(int(p.get('P_MIN_' + k, 0)) for k in ('SETUP', 'WARM', 'COLD', 'UPDATES')), 'updates': p.get('P_MIN_SECTION', 0)}.get(s, p.get('P_MIN_' + s.upper(), 0)))
    total = sum(mins(s) for s, _, _ in RELEASE_STAGES)
    cached = total - mins('store') + int(p.get('P_MIN_STORE_CACHED', 0))
    out = ['| stage | minutes | what it times | what the release run changes against the standard pass, and why the number stays |', '|---|---:|---|---|']
    out += ['| `%s` | %d | %s | %s |' % (s, mins(s), what, RELEASE_WHY[s]) for s, what, _ in RELEASE_STAGES]
    out += ['| **all** | **%d** | `bench/bench release`: one stage after another, the plan checked against 120 minutes before anything starts | with the cells of the engines not under test in the cache of cells: **%d** minutes (ideal: 60) |' % (total, cached)]
    return out


def stage_table(p):
    total = sum(minutes(p, s) for s, _, _ in STAGES)
    out = ['| stage | minutes | what it times | engines | basis of the minutes |', '|---|---:|---|---|---|']
    out += ['| `%s` | %d | %s | %s | %s |' % (s, minutes(p, s), what, eng, BASIS[s]) for s, what, eng in STAGES]
    out += ['| **stages** | **%d** | `bench final --size standard --docs --remeasure-all`, one stage after another; the budget it checks is 125 minutes | every engine measured, none from the cache | |' % total]
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


def scratch_table(results):
    path = os.path.join(results, 'cases-scratch.tsv')
    if not os.path.exists(path):
        return []
    rows = [l.rstrip('\n').split('\t') for l in open(path) if l.strip() and not l.startswith('#')][1:]
    out = ['<details><summary>Scratch of the cases stage in MB: three small sizes and the line to 1,000,000 orders</summary>', '',
           '| case | 50,000 | 100,000 | 200,000 | fixed | per 1M orders | at 1M |', '|---|---:|---:|---:|---:|---:|---:|']
    out += ['| %s | %s |' % (r[0] if r[0].startswith('(') else '`%s`' % r[0], ' | '.join(format(int(x), ',') for x in r[1:])) for r in rows]
    return out + ['', '</details>']


def cases_table(results, p):
    times = read_times(os.path.join(results, 'cases-time.tsv'))
    if not times:
        return ['The seconds of each case are not measured yet: `bench/bench cases --docs` writes bench/results/cases-time.tsv.']
    one = {k: v for k, v in times.items() if not k.startswith('(')}
    extra = {k: v for k, v in times.items() if k.startswith('(')}
    rows = sorted({v[0] for v in one.values()})
    n = max(rows, key=lambda r: sum(1 for v in one.values() if v[0] == r))
    one = {k: v for k, v in one.items() if v[0] == n}
    # fsum: the same total on every Python (sum() of floats is compensated since 3.12, so a total near half a second rounded differently in the lint job, 3.9)
    build, run = math.fsum(v[1] for v in one.values()), math.fsum(v[2] for v in one.values())
    fixed = math.fsum(v[1] for v in extra.values())
    out = ['%d cases at %s orders on one backend: %.0f s of table builds (a case that reads the table of an earlier one has 0), %.0f s of timed runs (%s rounds of 1 untimed and 5 timed runs on every lane, a lane stops repeating after %s s: the preset of the pass), '
           '%.0f s to generate the orders: **%.0f minutes** for the 88 cases of that run; the generic cases add about 2 more, and the stage allows 16.' % (len(one), format(n, ','), build, run, p.get('P_CASE_ROUNDS', 3), p.get('P_LANE_BUDGET', 20), fixed, (build + run + fixed) / 60), '',
           '<details><summary>Seconds of each case: build and timed runs</summary>', '', '| case | build (s) | timed runs (s) |', '|---|---:|---:|']
    out += ['| `%s` | %.1f | %.1f |' % (k, v[1], v[2]) for k, v in sorted(one.items(), key=lambda kv: -(kv[1][1] + kv[1][2]))]
    return out + ['', '</details>']


def block(results):
    p = presets()
    st, total = stage_table(p)
    win, full = window_table(results, total)
    out = ['Minutes of the release window on the reference host: the stages of `bench/bench final --size standard` (`bench/presets.env`), then the runs at 10,000,000 orders and the build of the extension. '
           'The window is **%d minutes** against the one to two hours the release has for every engine together. Which minutes are measured and which are arithmetic is in the last column: only the cases stage has been run since the cuts of the preset, '
           'the others are the stored times of the pass of 2026-10-22 (a test host) with the cuts applied, to be replaced by the first pass on the release host.' % full, ''] + st
    out += ['', win[0], win[1]] + win[2:]
    out += ['', '**Done ahead, outside the window.** The data (`bench store prepare`: the CSV of 1,000,000 and 10,000,000 orders; every result records the hash of the data it used) and the pulled images of the other engines '
            'do not depend on the commit under test and are made the day before (`bench refresh --prepare` builds the image and generates the data, bench/README.md); only the extension is built in the window.',
            '', '**What the 10M runs are for, and what dropping each would lose.** The store at 10,000,000 orders is the only evidence of the slope from 25,000 to 10,000,000 orders (the README\'s scale chart and its table of seven questions at 10M against '
            'every engine): it stays at 10M, with the cold runs, restarts and updates off. pg_duckdb at 10M stays too: between 1M and 10M the ratio of its Parquet lane to pg_sift halves on two of the ten questions (`orders_per_country` 10.7x to 5.2x, `search_newest` 27.9x to 8.2x) and moves within 1.4x on the other eight, so a 1M-only run would state a ratio for a size where it no longer holds. '
            'What moves to 1M instead of 10M is everything else: the cases (86 at 1M, 100,000 to 1M in the slope runs), the update section (200,000 and 1M) and the cold runs.',
            '', '**Engines at the same time: not in the window.** The timed windows of the engines run one after another (`--parallel-engines` stays 1, see "Engines at the same time" above): neighbours on other cores change a median through the memory bandwidth and the L3 they share. '
            'What can overlap is untimed work, and it is small: the loads of the engines (32 s at 1M) and the preparation above, which is already outside. ClickHouse\'s lightweight DELETE (336 s of the update section) is a timed phase of a published result, so it is not overlapped; it stays.']
    out += ['', 'What stays out of the window, and what it costs when a release needs it:', '', '| what | covers | command and time |', '|---|---|---|']
    out += ['| %s | %s | %s |' % o for o in OUTSIDE]
    e = engines_table(results)
    if e:
        out += ['', 'By engine, from the store pass of bench/results/store.json (1,000,000 orders, 3 CPUs):', ''] + e
    s = search_table(results)
    if s:
        out += ['', 'Search (slim tier, bench/results/articles.tsv and places.tsv):', ''] + s
    out += ['', 'Cases:', ''] + cases_table(results, p)
    out += [''] + scratch_table(results)
    out += ['', '#### The release run: one command', '',
            '`bench/bench release` is the one authoritative run after a release: `bench/bench final --size release --docs` with the checks of a run that publishes numbers (a clean checkout, the plan against a budget of 120 minutes before anything starts), '
            'and it covers every table the docs print. `--dry-run` prints the plan and the preconditions; `--rehearsal` runs every stage at the smoke size in a throwaway worktree with its own work and store directories and checks that the tables render. '
            'The preset `release` is `standard` with the repetitions cut where the stored runs show that no published number depends on them, and without the sweeps that only repeat a curve:', ''] + release_table()
    out += ['', 'Memory and disk of the cases stage: the clusters of the cases are scratch on tmpfs with fsync off (`CASES_TMP`, default `/dev/shm/sift-cases`; the stored timings and the minutes above were taken that way, so the numbers stay comparable). '
            'tmpfs is charged to the memory cap of the server, `CASES_MEM` (16G in this run: 8G was killed by the kernel at 6.3 GB of scratch). The plan of `bench/bench cases --plan --rows 1000000` says 5 GB; '
            'the line through three small sizes (table above, 18 cases) puts the cases it sampled at about 4.8 GB at 1,000,000 orders, 1.4 GB of it fixed, and the full list at 50,000 orders peaked at 6.3 GB where those 18 peak at 1.5 GB, '
            'so the other datasets add about 4.8 GB that do not grow with the orders: about 10 GB at 1,000,000 orders for the 211 cases (an estimate from the line, not a measurement at that size). '
            '`bench/bench release` reserves 3x the plan (15 GB) under `CASES_TMP` with `CASES_MEM=24G` when the host has 3x the plan and 8 GB of memory available (23 GB), else 2x (10 GB) with 16G, choosing by itself from the measured memory and printing the numbers; it checks the free space, the available memory (the reserve and 8 GB) and the cap (the reserve and 4 GB) before it starts, and stops with the numbers when they are short; '
            '`CASES_TMP=/path` moves the clusters to a disk (slower builds, minutes not comparable) and `CASES_MEM` changes the cap.']
    out += ['', '#### The go: one block and a checklist', '',
            '`HOST` is the ssh name, `DIR` a checkout with the engines built by `bench/bench setup`, `TAG` the release tag (`v3.7.0`; the run is of a tag, never of a commit id, because the history is rewritten before a release). The first line reads, the second starts, the third follows:', '', '```',
            "ssh HOST 'cd DIR && git fetch -q --tags && git checkout --detach TAG && bench/bench release --dry-run'",
            "ssh HOST 'cd DIR && nohup bench/bench release > ~/release-run.log 2>&1 & echo $! > ~/release-run.pid'",
            "ssh HOST 'tail -f ~/release-run.log'", '```', '',
            'Read in the dry-run, before the second line:', '',
            '1. The plan total is at most 120 minutes (the run refuses a plan over it). The `store` line says about 5 minutes when the cache of cells of this host exists and about 24 when everything is measured; `generic` is carried (the table says which tag measured it) when its inputs key is unchanged. No cache and no carry is the worst case, about 103 minutes in all; the cache and the carry together about 70.',
            '2. Every precondition is `ok`. A `MISSING` line (the ClickHouse binary, a port, a leftover container of `bench/bench store clean`, the engines of `bench/bench setup`) stops the run: do not start.',
            '3. The line `cases memory` shows the plan, the available memory and the reserve it chose by itself (3x the plan with `CASES_MEM=24G` when the host has 3x the plan and 8 GB available, else 2x with 16G); `the cases fit` is `ok`.',
            '4. The host is quiet: no other benchmark or heavy job (CPU and IO pressure `some avg10` under 10% in `/proc/pressure`). The run holds the perf lock for the whole pass; nothing else may time on the host.', '',
            'Abort (`ssh HOST \'kill $(cat ~/release-run.pid); cd DIR && bench/bench store clean\'`) on: a `MISSING` precondition or a plan over 120 minutes (the run stops by itself); two `OVER BUDGET` stage lines in a row or more than 120 minutes of wall time; less than 5 GB free on the disk of the host; another job starting there. A stage that fails does not stop the pass: the others continue, the log ends with `FAILED stages: ...`, and `bench/bench resume` runs only those.', '',
            'It is finished when the log ends with the table of stage minutes and `release run of TAG: N min wall, exit 0` and `pgrep -f bench/steps/final.sh` finds nothing on the host. A `FAILED stages:` line or an exit other than 0 is not a clean finish.', '',
            'The docs diff lands in the working tree of `DIR` on the host (README.md, bench/results, docs/img); nothing is committed there. '
            'The owner of the bench tables copies it (`ssh HOST \'cd DIR && git diff --binary\' | git apply` on a branch of the integration tip), runs `scripts/lint.sh readme-tables docs bench-fresh neutral` (a sentence that quotes an old number is listed there with "the results say"; the sift_table sizes and the `sift_table_ord` lane arrive in the same diff, every pg_sift cell being measured in the pass), and opens one pull request, "bench: results: the release run of TAG", which is landed with `mergeup-merge.sh` once the user has read the diff.']
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
