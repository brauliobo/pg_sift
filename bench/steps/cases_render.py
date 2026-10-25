"""The "Benchmark cases" block of README.md (the chart, the takeaway and the application-shaped cases) and docs/BENCHMARKS.md (every case), from bench/results/cases.tsv (bench/bench cases --docs).

    python3 -I bench/steps/cases_render.py [--results DIR] [--readme FILE] [--benchmarks FILE]      (bench/bench readme)

Every number is a row of the results file; the shape text, the baselines, the variants and the headline marks are the registry's (bench/cases.tsv). A case where pg_sift is
slower than PostgreSQL or ties it stays in the table and is named in the takeaway.
"""
import argparse
import math
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import cases as cases_mod  # noqa: E402
import extras  # noqa: E402

REPO = os.path.dirname(os.path.dirname(HERE))
TIE = 0.05
OFF = ('scan', 'bulk_off', 'runs_off', 'zone_off')   # the variants that are the optimization's own setting turned off
UNIT = {'ms': 'ms', 'wal_bytes': 'WAL bytes per row', 'cpu_ms': 'CPU ms'}
VARIANT = {'agg_forced': 'the aggregate scan forced whatever the plans cost (`sift.agg_force = on`)', 'scan': 'the units scanned (`sift.agg_preagg = off`)', 'runs_off': 'the sorted runs not read (`sift.topn_runs = off`)', 'zone_off': 'the zones of the keys after the first not used (`sift.topn_zone_keys = off`)',
           'forced': 'the top-N scan forced by turning the btree plans off (`enable_indexscan`, `enable_indexonlyscan`, `enable_bitmapscan`)', 'bulk_off': 'the same `sift_table` and COPY with write buffers instead of column groups (`sift.table_bulk_groups = off`), promotion included'}


def figure(v):
    return 'n/a' if v is None else extras.ms(v)


def speed(r):
    return '%.0fx' % r if r >= 9.5 else '%.1fx' % r


def collect(rows, registry, root=REPO):
    """per (case, metric): the native and sift values, the others by arm, the plan and whether the host was busy"""
    out = []
    for c in registry:
        for m in c.metrics:
            get = lambda arm: extras.get(rows, case=c.id, arm=arm, metric=m)  # noqa: E731
            n, s = get('native'), get('sift')
            if not n or not s:
                continue
            val = lambda r: float(r['value']) if r and r['value'] not in ('', 'NA', 'error') else None  # noqa: E731
            others = {arm: ('not supported' if (r := get(arm)) and r['value'] == 'NA' else val(r)) for arm in c.baselines[1:] + [v[0] for v in c.variants] if get(arm)}
            forced, why = get(cases_mod.FORCED[0]), get('why')
            out.append(dict(case=c, metric=m, native=val(n), sift=val(s), others=others, plan=re.sub(r' NOISY$', '', s['note']), kept=c.fallback, noisy=any('NOISY' in (r['note'] or '') for r in (n, s)),
                            forced=val(forced), forced_note=re.sub(r' NOISY$', '', forced['note']) if forced else '', why=why['note'] if why else ''))
    return out


def gap(f):
    """what the scan does where the planner keeps PostgreSQL's plan (the lane with `sift.agg_force = on`): cost (faster than PostgreSQL, the cost model declines it), declined (not faster:
    the planner is right), feature (the shape cannot run on the scan), unmeasured"""
    if f['forced_note'].startswith('no scan'):
        return 'feature'
    if not f['forced'] or not f['native']:
        return 'unmeasured'
    return 'cost' if f['native'] / f['forced'] >= 1 + TIE else 'declined'


def declined(f):
    """the speed-up cell of a case whose default plan is PostgreSQL's: no ratio of that plan against itself, the ratio of the forced scan"""
    g = gap(f)
    if g == 'feature':
        return 'default: PostgreSQL\'s plan; the scan cannot run it'
    if g == 'unmeasured':
        return 'default: PostgreSQL\'s plan; forced scan not measured'
    r = f['native'] / f['forced']
    return 'default: PostgreSQL\'s plan; forced scan **%s**' % speed(r) if g == 'cost' else 'default: PostgreSQL\'s plan; forced scan %s' % ('%s slower' % speed(1 / r) if r <= 1 - TIE else 'tie (%s)' % speed(r))


def ranked(f):
    """0 a loss or a gap (the scan slower than PostgreSQL, or a case where the planner declines it and the forced scan wins or cannot run), 1 a tie or no ratio, 2 a win"""
    r = f['native'] / f['sift'] if f['native'] and f['sift'] else 1
    if f['kept']:
        return 0 if gap(f) in ('cost', 'feature') else 1
    return 0 if r <= 1 - TIE else 2 if r >= 1 + TIE else 1


def verdict(f):
    if f['kept'] or not f['native'] or not f['sift']:
        return None
    return f['native'] / f['sift']


def cell_others(f):
    parts = []
    for arm, v in f['others'].items():
        parts.append('%s %s' % ('`%s`' % arm if arm in VARIANT else arm, 'no scan' if arm == cases_mod.FORCED[0] and gap(f) == 'feature' else v if isinstance(v, str) else figure(v)))
    return '; '.join(parts) or '-'


def by_gap(figs):
    """the cases where the planner keeps PostgreSQL's plan, by gap(): cost, declined, feature, unmeasured -> case ids"""
    out = {}
    for f in figs:
        if f['kept'] and f['metric'] == f['case'].metrics[0]:
            out.setdefault(gap(f), []).append(f['case'].id)
    return out


def kept_text(figs, ids=True):
    """the sentence on the cases that run PostgreSQL's plan: no ratio of that plan against itself, what the scan does when it is forced"""
    g = by_gap(figs)
    total = len({f['case'].id for f in figs})
    k = sum(map(len, g.values()))
    if not k:
        return 'the planner chose the scan on all %d cases' % total
    names = lambda key: ' (%s)' % ', '.join(g[key]) if ids else ''  # noqa: E731
    parts = ['faster on %d%s: the cost model declines it' % (len(g['cost']), names('cost')) if g.get('cost') else '', 'not faster on %d%s: the planner is right' % (len(g['declined']), names('declined')) if g.get('declined') else '',
             'cannot run on %d%s: the shape is not supported' % (len(g['feature']), names('feature')) if g.get('feature') else '', 'not measured on %d%s' % (len(g['unmeasured']), names('unmeasured')) if g.get('unmeasured') else '']
    return 'the planner chose the scan on %d of %d cases and ran PostgreSQL\'s plan on %d (no ratio for those); with the scan forced (`sift.agg_force`) it is %s' % (total - k, total, k, '; '.join(p for p in parts if p))


def takeaway(figs, brief=False):
    rs = [(f, verdict(f)) for f in figs if verdict(f)]
    faster = [r for f, r in rs if r >= 1 + TIE]
    tie = [f for f, r in rs if 1 - TIE < r < 1 + TIE]
    slower = [(f, 1 / r) for f, r in rs if r <= 1 - TIE]
    lo, hi = (speed(min(faster)), speed(max(faster))) if faster else ('', '')
    text = 'where the planner chooses the scan, pg_sift is faster than PostgreSQL on the same rows (`sift.enable_agg = off`) on %d of %d numbers%s' % (len(faster), len(rs), ' (%s to %s)' % (lo, hi) if faster else '')
    if tie:
        text += ', a tie on %d' % len(tie)
    if slower:
        text += ', slower on %d: %s' % (len(slower), ', '.join('%s %s (%s slower)' % (f['case'].id, UNIT[f['metric']], speed(r)) for f, r in slower))
    flat = [(f, a) for f, _ in rs for a in OFF if isinstance(f['others'].get(a), float) and f['sift'] and f['others'][a] <= f['sift'] * (1 + TIE)]
    if brief:
        return text + '; ' + kept_text(figs, ids=False) + '. The gap list per case and the setting of a variant that brings no gain are in docs/BENCHMARKS.md.'
    if flat:
        text += '; the setting itself brings no gain (its variant is as fast, within %d%%) on %d: %s' % (TIE * 100, len(flat), ', '.join('%s %s `%s`' % (f['case'].id, UNIT[f['metric']], a) for f, a in flat))
    return text + '; ' + kept_text(figs) + '.'


def gap_lines(figs):
    """docs/BENCHMARKS.md, block gaps: one row per case that runs PostgreSQL's plan, with the lane that forces the scan beside it and the reasons sift.explain_why gives; the key_sets work from this list"""
    owner = {'cost': 'cost model', 'feature': 'feature breadth', 'declined': 'none (the planner is right)', 'unmeasured': '-'}
    rows = [f for f in figs if f['kept'] and f['metric'] == f['case'].metrics[0]]
    if not rows:
        return ['No case runs PostgreSQL\'s plan: the planner chooses the scan on every case.']
    rows.sort(key=lambda f: (['cost', 'feature', 'declined', 'unmeasured'].index(gap(f)), f['case'].id))
    why = lambda t: t.replace('|', '\\|') or '-'  # noqa: E731
    head = ['| case | shape | gap | owner | PostgreSQL | forced scan | forced scan vs PostgreSQL | why the planner declines (`sift.explain_why`, default) | why the scan cannot run (forced) |', '|---|---|---|---|---:|---:|---:|---|---|']
    body = ['| `%s` | %s | %s | %s | %s | %s | %s | %s | %s |' % (f['case'].id, f['case'].label.replace('|', '\\|'), {'cost': 'cost-model gap', 'feature': 'feature gap', 'declined': 'planner right', 'unmeasured': 'not measured'}[gap(f)],
            owner[gap(f)], figure(f['native']), *(('-', '-') if gap(f) in ('feature', 'unmeasured') else (figure(f['forced']), speed(f['native'] / f['forced']))), why(f['why']),
            why(f['forced_note'][len('no scan: '):]) if gap(f) == 'feature' else '-') for f in rows]
    return ['Where the planner keeps PostgreSQL\'s plan the case is not a comparison of pg_sift with PostgreSQL (the same plan against itself): the default lane is listed without a ratio and a second pg_sift lane forces the scan (`sift.agg_force = on`).', '',
            'A **cost-model gap** is a case where the forced scan beats PostgreSQL and the planner still declines it; a **feature gap** a shape the scan cannot run (the reason is the one `sift.explain_why` gives); **planner right**: the forced scan is not faster.', ''] + head + body


def rounds(meta):
    m = re.search(r'(\d+) rounds of 1 untimed and (\d+) timed runs', meta.get('method', ''))
    return '%s rounds of %s interleaved timed runs' % m.groups() if m else 'the timed runs'


def lines(meta, rows, registry, results_dir='bench/results', bench_dir='bench'):
    figs = collect(rows, registry)
    if not figs:
        return ['Not measured at this build yet: `bench/bench cases --docs` writes this block.']
    n = len({f['case'].id for f in figs})
    worst = {}
    for f in figs:
        worst[f['case'].id] = min(worst.get(f['case'].id, 2), ranked(f))
    figs = sorted(figs, key=lambda f: worst[f['case'].id])   # losses first, then ties and kept plans, then the wins, each in the registry's order
    head = ['| case | shape | unit | PostgreSQL | pg_sift | other engines and variants | speed-up | plan |', '|---|---|---|---:|---:|---|---:|---|']
    body = []
    for f in figs:
        r = verdict(f)
        sp = declined(f) if f['kept'] else '-' if r is None else '**%s**' % speed(r) if r >= 1 + TIE else '%s (slower)' % speed(1 / r) if r <= 1 - TIE else 'tie (%s)' % speed(r)
        if r and isinstance(f['others'].get('bulk_off'), float) and f['sift']:
            sp += ' (`sift.table_bulk_groups` on vs off: %s lower)' % speed(f['others']['bulk_off'] / f['sift'])
        first = f['metric'] == f['case'].metrics[0]
        body.append('| `%s` | %s | %s | %s | %s | %s | %s | %s |' % (f['case'].id, f['case'].label.replace('|', '\\|') if first else '(the same case)', UNIT[f['metric']], figure(f['native']), 'PostgreSQL\'s plan' if f['kept'] else figure(f['sift']), cell_others(f), sp,
                                                                  ('`%s`' % f['plan'].replace('|', '\\|')) if len(f['plan']) < 90 else '`%s`...' % f['plan'][:86]))
    used = {k for f in figs for k in f['others'] if k in VARIANT}
    noisy = [f['case'].id for f in figs if f['noisy']]
    out = ['%d benchmark cases (`bench/cases.tsv`) on %s rows (the orders of the orders benchmark; the cases on `events` use the plan-choice gate\'s events, dims and facts, `test/plan/gen.sql`), one backend with no parallel workers, median of %s; lower is better, the speed-up is the PostgreSQL figure over the pg_sift one. %s' % (
        n, '{:,}'.format(int(meta.get('orders', 0))), rounds(meta), extras.footer(meta)), ''] + head + body + [
        '', '**Takeaway:** ' + takeaway(figs), '', '**Caveats**', '',
        '- PostgreSQL is the same table, indexes and statement with `sift.enable_agg = off` (the btrees of the store\'s PostgreSQL lane are there); pg_duckdb is a container with a plain copy of the rows (`n/a`: it does not run the case, `not supported`: DuckDB cannot execute the statement).',
        '- The plan column is what the pg_sift lane\'s EXPLAIN showed; a case whose plan is not the expected one, or whose answer differs from PostgreSQL\'s, stops the run.']
    out += ['- Variants: ' + '; '.join('`%s` is %s' % (k, VARIANT[k]) for k in sorted(used)) + '.'] * bool(used)
    out += ['- Measured while the host was busy (the load average is in each row of the results): %s. Read the ratios, not the milliseconds.' % ', '.join(sorted(set(noisy)))] * bool(noisy)
    return out + ['', 'Raw results: [%s/cases.tsv](%s/cases.tsv); the cases: [%s/cases.tsv](%s/cases.tsv); reproduce with `bench/bench cases` (`--docs --rows 1000000` for the published size).' % (results_dir, results_dir, bench_dir, bench_dir)]


def size(n):
    return '%.3gM' % (n / 1e6) if n >= 1e6 else '%.3gk' % (n / 1e3) if n >= 1e3 else str(n)


def curves(rows, registry):
    """per case that has scale rows (case id ID@ROWS): the points by size, each with PostgreSQL's and pg_sift's time (the metric ms), the plans and whether the host was busy"""
    out = []
    for c in registry:
        m = 'ms' if 'ms' in c.metrics else c.metrics[0]
        pts = []
        for n in sorted({int(r['case'].partition('@')[2]) for r in rows if r['case'].partition('@')[0] == c.id and '@' in r['case']}):
            get = lambda arm: extras.get(rows, case='%s@%d' % (c.id, n), arm=arm, metric=m)  # noqa: E731
            nat, sf = get('native'), get('sift')
            if nat and sf and nat['value'] not in ('', 'NA', 'error') and sf['value'] not in ('', 'NA', 'error'):
                pts.append(dict(rows=n, native=float(nat['value']), sift=float(sf['value']), kept=re.sub(r' NOISY$', '', nat['note']) == re.sub(r' NOISY$', '', sf['note']),
                                noisy=any('NOISY' in (r['note'] or '') for r in (nat, sf)), plan=re.sub(r' NOISY$', '', sf['note'])))
        if pts:
            out.append(dict(case=c, metric=m, points=pts))
    return out


def ratio(pt):
    return pt['native'] / pt['sift'] if pt['sift'] else float('inf')


def crossover(pts):
    """the size from which pg_sift is faster than PostgreSQL and stays faster: from the smallest point, or an estimate between the last point it loses or ties at and the first it wins at
    (a straight line through the two ratios on logarithmic axes), or none up to the largest point, with the way the ratio is going"""
    wins = [ratio(p) >= 1 + TIE and not p['kept'] for p in pts]
    if pts[-1]['kept']:
        return 'the planner keeps PostgreSQL\'s plan up to %s rows' % size(pts[-1]['rows'])
    if all(wins):
        return 'wins from %s rows' % size(pts[0]['rows'])
    if not wins[-1]:
        heading = ''
        if len(pts) > 1 and ratio(pts[-1]) != ratio(pts[-2]):
            heading = ', the ratio is %s' % ('rising' if ratio(pts[-1]) > ratio(pts[-2]) else 'falling')
        return 'none up to %s rows (%s at the largest%s)' % (size(pts[-1]['rows']), speed(ratio(pts[-1])) + ' faster' if ratio(pts[-1]) >= 1 else speed(1 / ratio(pts[-1])) + ' slower', heading)
    i = max(k for k, w in enumerate(wins) if not w)
    a, b = pts[i], pts[i + 1]
    ra, rb = ratio(a), ratio(b)
    if ra > 0 and rb > 0 and ra != rb:
        at = math.exp(math.log(a['rows']) + (0 - math.log(ra)) / (math.log(rb) - math.log(ra)) * (math.log(b['rows']) - math.log(a['rows'])))
        return 'about %s rows (between %s and %s, wins from %s)' % (size(min(max(at, a['rows']), b['rows'])), size(a['rows']), size(b['rows']), size(b['rows']))
    return 'wins from %s rows' % size(b['rows'])


def scale_lines(meta, rows, registry):
    cs = curves(rows, registry)
    if not cs:
        return ['Not measured at scale yet: `bench/bench cases --scales 1,10,100 --docs` writes this block (one backend, on a host with the memory for the largest point; `bench/bench refresh --stages scale`).']
    sizes = sorted({p['rows'] for c in cs for p in c['points']})
    cs.sort(key=lambda c: (0 if ratio(c['points'][-1]) <= 1 - TIE else 2 if ratio(c['points'][-1]) >= 1 + TIE and not c['points'][-1]['kept'] else 1))   # losses at the largest size first
    head = ['| case | shape | ' + ' | '.join('%s rows (%gx)' % (format(n, ','), n / int(meta.get('orders', n))) for n in sizes) + ' | crossover |', '|---|---|' + '---:|' * len(sizes) + '---|']
    body = []
    for c in cs:
        cells = []
        for n in sizes:
            p = next((p for p in c['points'] if p['rows'] == n), None)
            if p is None:
                cells.append('-')
                continue
            r = ratio(p)
            verdict_ = 'PostgreSQL\'s plan' if p['kept'] else '**%s**' % speed(r) if r >= 1 + TIE else '%s slower' % speed(1 / r) if r <= 1 - TIE else 'tie'
            cells.append('%s / %s ms %s%s' % (figure(p['native']), figure(p['sift']), verdict_, ' (busy)' if p['noisy'] else ''))
        body.append('| `%s` | %s | %s | %s |' % (c['case'].id, c['case'].label.replace('|', '\\|'), ' | '.join(cells), crossover(c['points'])))
    top = [c for c in cs if ratio(c['points'][-1]) <= 1 - TIE]
    win = [c for c in cs if ratio(c['points'][-1]) >= 1 + TIE and not c['points'][-1]['kept']]
    text = 'at the largest size (%s rows) pg_sift is faster than PostgreSQL on %d of %d cases' % (format(sizes[-1], ','), len(win), len(cs))
    if top:
        text += ', slower on %d: %s' % (len(top), ', '.join('%s (%s slower)' % (c['case'].id, speed(1 / ratio(c['points'][-1]))) for c in top))
    kept = [c for c in cs if c['points'][-1]['kept'] and ratio(c['points'][-1]) > 1 - TIE]
    if kept:
        text += '; the planner kept PostgreSQL\'s plan on %d: %s' % (len(kept), ', '.join(c['case'].id for c in kept))
    return ['The cases of the table above at %s orders (%s at 1x), the planner\'s own plan at each size (a case whose plan changes with the size shows it in the ratio), time in ms of PostgreSQL (`sift.enable_agg = off`, same table and indexes) / pg_sift, '
            'one backend, median of %s. %s' % (', '.join(size(n) for n in sizes), format(int(meta.get('orders', 0)), ','), rounds(meta), extras.footer(meta)), ''] + head + body + [
        '', '**Takeaway:** ' + text + '.', '',
        '**Caveats**', '',
        '- The largest sizes are measured once on the host named above, not on a laptop: the milliseconds are that host\'s, the ratios and the crossover are what travels. `(busy)` marks a cell measured while the host was busy.',
        '- The crossover of a case is the size from which pg_sift is faster by more than 5% and stays so; between two points it is a straight line through the two ratios on logarithmic axes, an estimate and not a measurement.',
        '', 'Raw results: [bench/results/cases-scale.tsv](bench/results/cases-scale.tsv); reproduce with `bench/bench cases --scales 1,10,100 --docs`.']


def featured(rows, registry, scale_rows):
    """the cases the registry marks `headline`, the ones that read like an application's query: a narrow table with the log-log slope of each engine over the sizes of cases-scale.tsv when it has them"""
    import bench_charts
    figs = {f['case'].id: f for f in collect(rows, registry) if f['metric'] == f['case'].metrics[0]}
    ser = {}
    for r in scale_rows:
        cid, _, n = r['case'].partition('@')
        if r['metric'] == 'ms' and r['arm'] in ('native', 'sift') and r['value'] not in ('', 'NA', 'error') and n:
            ser.setdefault(cid, {}).setdefault(r['arm'], []).append((int(n), float(r['value'])))
    head = ['case', 'what it stresses', 'PostgreSQL ms', 'pg_sift ms', 'speed-up', 'slope PostgreSQL / pg_sift']
    flips = plan_flips(scale_rows)
    body = []
    for c in registry:
        f = figs.get(c.id)
        if not c.headline or not f or c.metrics[0] != 'ms':
            continue
        r = verdict(f)
        sp = declined(f) if f['kept'] else '**%s**' % speed(r) if r and r >= 1 + TIE else '%s slower' % speed(1 / r) if r and r <= 1 - TIE else 'tie'
        sl = ['%.2f' % bench_charts.slope(*zip(*sorted(ser[c.id][a]))) if len(ser.get(c.id, {}).get(a, [])) >= 3 else '-' for a in ('native', 'sift')]
        body.append(['[`%s`](bench/cases/%s.sql)' % (c.id, c.id), c.label.replace('|', '\\|'), figure(f['native']), "PostgreSQL's plan" if f['kept'] else figure(f['sift']), sp, ' / '.join(sl)])
    return (extras.table(head, body, ['---', '---', '---:', '---:', '---:', '---:']) + ([''] + flips if flips else [])) if body else []


def plan_flips(scale_rows):
    """a sentence on the cases whose plan changed with the size: pg_sift kept PostgreSQL's plan at some sizes and ran its scan at others"""
    by = {}
    for r in scale_rows:
        cid, _, n = r['case'].partition('@')
        if r['metric'] == 'ms' and r['arm'] in ('native', 'sift') and n:
            by.setdefault(cid, {}).setdefault(int(n), {})[r['arm']] = re.sub(r' NOISY$', '', r['note'] or '')
    sizes = {n for c in by.values() for n in c}
    flipped = sorted(cid for cid, c in by.items() if len({v.get('native') == v.get('sift') for v in c.values()}) > 1)
    if not by:
        return []
    return ['Between %s and %s rows the planner changed its plan on %d of the %d cases measured at scale%s.' % (size(min(sizes)), size(max(sizes)), len(flipped), len(by), ': ' + ', '.join('`%s`' % c for c in flipped) if flipped else '')]


def readme_lines(meta, rows, registry, scale_rows, results_dir='bench/results'):
    """the README's block: the chart of every case, the table of the application-shaped ones, the takeaway; the machine and method are the footnote [^cases]"""
    figs = collect(rows, registry)
    if not figs:
        return ['Not measured at this build yet: `bench/bench cases --docs` writes this block.']
    n = len({f['case'].id for f in figs})
    out = ['%d cases, each a query shape the planner has to recognize (a report with grouping sets, a ranked top-N per group, an anti join, a keyset page, a lookup by key, a JSON array and more), on %s rows, one backend[^cases]. '
           'Every case runs on pg_sift and on PostgreSQL on the same table and indexes (`sift.enable_agg = off`) and every answer is compared before it is timed.' % (n, '{:,}'.format(int(meta.get('orders', 0)))), '',
           '<picture><source media="(prefers-color-scheme: dark)" srcset="docs/img/cases-speedup-dark.svg"><img alt="Speed-up of pg_sift over PostgreSQL for every benchmark case, by family" src="docs/img/cases-speedup-light.svg"></picture>', '',
           '**Takeaway:** ' + takeaway(figs, brief=True), '']
    table = featured(rows, registry, scale_rows)
    if table:
        out += ['The cases that read like an application\'s query, with the SQL in `bench/cases/` and the slope of time against rows over the sizes of [cases-scale.tsv](%s/cases-scale.tsv) (1.0: time proportional to the rows):' % results_dir, ''] + table + ['']
    out += ['<picture><source media="(prefers-color-scheme: dark)" srcset="docs/img/cases-ecdf-dark.svg"><img alt="Share of cases at least as fast as x times PostgreSQL" src="docs/img/cases-ecdf-light.svg"></picture>', '',
            'Every case with its plan and variants: [docs/BENCHMARKS.md](docs/BENCHMARKS.md#benchmark-cases); raw results [%s/cases.tsv](%s/cases.tsv); the cases [bench/cases.tsv](bench/cases.tsv); reproduce with `bench/bench cases --docs --rows 1000000`.' % (results_dir, results_dir), '',
            '[^cases]: %s Median of %s; PostgreSQL %s, pg_sift %s, `work_mem` %s, no parallel workers; host load at the start %s. Where the planner keeps PostgreSQL\'s plan the bar is gray.' % (
                meta.get('cpu', '?') + '.', rounds(meta), meta.get('postgresql', '?'), meta.get('pg_sift', '?'), meta.get('work_mem', '64MB'), meta.get('host_load_before', '?'))]
    return out


def render(results, readme, benchmarks):
    path = os.path.join(results, 'cases.tsv')
    registry = cases_mod.read_registry()
    if os.path.exists(path):
        meta, rows = extras.read(path)
        scale_path = os.path.join(results, 'cases-scale.tsv')
        scale_rows = extras.read(scale_path)[1] if os.path.exists(scale_path) else []
        for doc, body in ((benchmarks, lines(meta, rows, registry, 'results', '../bench')), (readme, readme_lines(meta, rows, registry, scale_rows))):
            if doc and os.path.exists(doc) and '<!-- bench:faster33:begin' in open(doc).read():
                extras.replace_block(doc, 'faster33', body)
        if benchmarks and os.path.exists(benchmarks) and '<!-- bench:gaps:begin' in open(benchmarks).read():
            extras.replace_block(benchmarks, 'gaps', gap_lines(collect(rows, registry)))
    scale = os.path.join(results, 'cases-scale.tsv')
    meta, rows = extras.read(scale) if os.path.exists(scale) else ({}, [])
    for doc in (benchmarks, readme):
        if doc and os.path.exists(doc) and '<!-- bench:scale33:begin' in open(doc).read():
            body = scale_lines(meta, rows, registry)
            extras.replace_block(doc, 'scale33', [l.replace('bench/results/', 'results/') if doc == benchmarks else l for l in body])


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--results', default=os.path.join(REPO, 'bench', 'results'))
    ap.add_argument('--readme', default=os.path.join(REPO, 'README.md'))
    ap.add_argument('--benchmarks', default=os.path.join(REPO, 'docs', 'BENCHMARKS.md'))
    a = ap.parse_args()
    render(a.results, a.readme, a.benchmarks)


if __name__ == '__main__':
    main()
