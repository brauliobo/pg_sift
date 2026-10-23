"""The "What got faster in 3.3" block of README.md and docs/BENCHMARKS.md, and its line in "Performance highlights", from docs/results/cases.tsv (bench/bench cases --docs).

    python3 -I bench/steps/cases_render.py [--results DIR] [--readme FILE] [--benchmarks FILE]      (bench/bench readme, scripts/check-bench-fresh.sh)

Every number is a row of the results file; the shape text, the baselines, the variants and the headline marks are the registry's (bench/cases.tsv). A case where pg_sift is
slower than PostgreSQL or ties it stays in the table and is named in the takeaway.
"""
import argparse
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
VARIANT = {'scan': 'the units scanned (`sift.agg_preagg = off`)', 'runs_off': 'the sorted runs not read (`sift.topn_runs = off`)', 'zone_off': 'the zones of the keys after the first not used (`sift.topn_zone_keys = off`)',
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
            out.append(dict(case=c, metric=m, native=val(n), sift=val(s), others=others, plan=re.sub(r' NOISY$', '', s['note']), kept=re.sub(r' NOISY$', '', n['note']) == re.sub(r' NOISY$', '', s['note']), noisy=any('NOISY' in (r['note'] or '') for r in (n, s))))
    return out


def same_plan(f):
    """the planner kept PostgreSQL's plan: no ratio of the optimization, but the cost of deciding, when it shows"""
    r = f['native'] / f['sift'] if f['native'] and f['sift'] else 1
    return 'same plan' + (' (%s slower)' % speed(1 / r) if r <= 1 - TIE else '')


def ranked(f):
    """0 a loss (the scan slower than PostgreSQL, or its time with the same plan), 1 a tie or no ratio, 2 a win"""
    r = f['native'] / f['sift'] if f['native'] and f['sift'] else 1
    return 0 if r <= 1 - TIE else 2 if r >= 1 + TIE and not f['kept'] else 1


def verdict(f):
    if f['kept'] or not f['native'] or not f['sift']:
        return None
    return f['native'] / f['sift']


def cell_others(f):
    parts = []
    for arm, v in f['others'].items():
        parts.append('%s %s' % ('`%s`' % arm if arm in VARIANT else arm, v if isinstance(v, str) else figure(v)))
    return '; '.join(parts) or '-'


def takeaway(figs):
    rs = [(f, verdict(f)) for f in figs if verdict(f)]
    kept = [f for f in figs if f['kept']]
    faster = [r for f, r in rs if r >= 1 + TIE]
    tie = [f for f, r in rs if 1 - TIE < r < 1 + TIE]
    slower = [(f, 1 / r) for f, r in rs if r <= 1 - TIE]
    lo, hi = (speed(min(faster)), speed(max(faster))) if faster else ('', '')
    text = 'pg_sift (the planner\'s own choice) is faster than PostgreSQL on the same rows (`sift.enable_agg = off`) on %d of %d numbers%s' % (len(faster), len(rs), ' (%s to %s)' % (lo, hi) if faster else '')
    if tie:
        text += ', a tie on %d' % len(tie)
    if slower:
        text += ', slower on %d: %s' % (len(slower), ', '.join('%s %s (%s slower)' % (f['case'].id, UNIT[f['metric']], speed(r)) for f, r in slower))
    flat = [(f, a) for f, _ in rs for a in OFF if isinstance(f['others'].get(a), float) and f['sift'] and f['others'][a] <= f['sift'] * (1 + TIE)]
    if flat:
        text += '; the setting itself brings no gain (its variant is as fast, within %d%%) on %d: %s' % (TIE * 100, len(flat), ', '.join('%s %s `%s`' % (f['case'].id, UNIT[f['metric']], a) for f, a in flat))
    if kept:
        text += '; the planner kept the normal plan (the same plan as PostgreSQL\'s, no ratio; the slower ones are pg_sift\'s time with that plan) on %d: %s' % (len(kept), ', '.join('%s %s%s' % (f['case'].id, UNIT[f['metric']], same_plan(f)[len('same plan'):]) for f in kept))
    return text + '.'


def rounds(meta):
    m = re.search(r'(\d+) rounds of 1 untimed and (\d+) timed runs', meta.get('method', ''))
    return '%s rounds of %s interleaved timed runs' % m.groups() if m else 'the timed runs'


def lines(meta, rows, registry, results_dir='docs/results', bench_dir='bench'):
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
        sp = same_plan(f) if f['kept'] else '-' if r is None else '**%s**' % speed(r) if r >= 1 + TIE else '%s (slower)' % speed(1 / r) if r <= 1 - TIE else 'tie (%s)' % speed(r)
        if r and isinstance(f['others'].get('bulk_off'), float) and f['sift']:
            sp += ' (`sift.table_bulk_groups` on vs off: %s lower)' % speed(f['others']['bulk_off'] / f['sift'])
        first = f['metric'] == f['case'].metrics[0]
        body.append('| `%s` | %s | %s | %s | %s | %s | %s | %s |' % (f['case'].id, f['case'].label.replace('|', '\\|') if first else '(the same case)', UNIT[f['metric']], figure(f['native']), figure(f['sift']), cell_others(f), sp,
                                                                  ('`%s`' % f['plan'].replace('|', '\\|')) if len(f['plan']) < 90 else '`%s`...' % f['plan'][:86]))
    used = {k for f in figs for k in f['others'] if k in VARIANT}
    noisy = [f['case'].id for f in figs if f['noisy']]
    out = ['%d cases of the 3.3 optimizations (`bench/cases.tsv`) on %s rows (the orders of the store benchmark; the cases on `events` use the plan-choice gate\'s events, dims and facts, `test/plan/gen.sql`), one backend with no parallel workers, median of %s; lower is better, the speed-up is the PostgreSQL figure over the pg_sift one. %s' % (
        n, '{:,}'.format(int(meta.get('orders', 0))), rounds(meta), extras.footer(meta)), ''] + head + body + [
        '', '**Takeaway:** ' + takeaway(figs), '', '**Caveats**', '',
        '- PostgreSQL is the same table, indexes and statement with `sift.enable_agg = off` (the btrees of the store\'s PostgreSQL lane are there); pg_duckdb is a container with a plain copy of the rows (`n/a`: it does not run the case, `not supported`: DuckDB cannot execute the statement).',
        '- The plan column is what the pg_sift lane\'s EXPLAIN showed; a case whose plan is not the expected one, or whose answer differs from PostgreSQL\'s, stops the run.']
    out += ['- Variants: ' + '; '.join('`%s` is %s' % (k, VARIANT[k]) for k in sorted(used)) + '.'] * bool(used)
    out += ['- Measured while the host was busy (the load average is in each row of the results): %s. Read the ratios, not the milliseconds.' % ', '.join(sorted(set(noisy)))] * bool(noisy)
    return out + ['', 'Raw results: [%s/cases.tsv](%s/cases.tsv); the cases: [%s/cases.tsv](%s/cases.tsv); reproduce with `bench/bench cases` (`--docs --rows 1000000` for the published size).' % (results_dir, results_dir, bench_dir, bench_dir)]


def headline(rows, registry):
    """the line of Performance highlights: the cases the registry marks as headlines, with the losses of the whole table said plainly"""
    figs = collect(rows, registry)
    heads = [f for f in figs if f['case'].headline and f['metric'] == f['case'].metrics[0] and verdict(f)]
    if not heads:
        return None
    parts = '; '.join('%s (%s%s)' % (f['case'].title, speed(verdict(f)), ' less WAL' if f['metric'] == 'wal_bytes' else '') for f in heads)
    slower = [f for f in figs if verdict(f) and verdict(f) <= 1 - TIE]
    return parts, [('%s %s' % (f['case'].id, UNIT[f['metric']]), speed(1 / verdict(f))) for f in slower]


def render(results, readme, benchmarks):
    path = os.path.join(results, 'cases.tsv')
    if not os.path.exists(path):
        return
    meta, rows = extras.read(path)
    registry = cases_mod.read_registry()
    for doc, body in ((benchmarks, lines(meta, rows, registry, 'results', '../bench')), (readme, lines(meta, rows, registry))):
        if doc and os.path.exists(doc) and '<!-- bench:faster33:begin' in open(doc).read():
            extras.replace_block(doc, 'faster33', body)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--results', default=os.path.join(REPO, 'docs', 'results'))
    ap.add_argument('--readme', default=os.path.join(REPO, 'README.md'))
    ap.add_argument('--benchmarks', default=os.path.join(REPO, 'docs', 'BENCHMARKS.md'))
    a = ap.parse_args()
    render(a.results, a.readme, a.benchmarks)


if __name__ == '__main__':
    main()
