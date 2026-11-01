# A run, a plan and a listing: the points, the results file and the times file.
# cmd_run is the command `cases.py run`.

import datetime
import os
import re
import subprocess
import sys
import time

import iolib
from duckdb import busy, wait_quiet

from . import runcfg, recs
from .runcfg import BUILT, DATASETS, HERE, IO_HOW, REPO, TIMES, die, log
from .registry import Case, lane_cpus, read_registry, workers_asked
from .server import Fresh, build, dataset, drop_added, lane_sessions, load_duck, psql, server_load
from .measure import PerfFailed, expand, host, io_load, measure_load, measure_query, minstr, stats, summarize


def release(c, left, rows, scaled):
    """drop the tables of C that no later case of the run reads (a copy of the table for each of the 30 cases at 20 million rows is not room the host has; nor is each dataset of a run at 1,000,000 rows)"""
    if c.share or c.dataset in DATASETS:
        left[c.schema()] -= 1
    if os.environ.get('CASES_KEEP'):
        return
    psql("SELECT format('DROP SCHEMA %%I CASCADE', nspname) FROM pg_namespace WHERE starts_with(nspname, 'c_%s__') \\gexec" % c.id)   # the variant copies a setup makes (c_ID__whole): not the case's own schema
    if c.load:   # the schema its statements write to (server.build)
        psql('DROP SCHEMA IF EXISTS c_%s CASCADE' % c.id)
    if c.dataset in DATASETS:
        if left[c.schema()] == 0:
            psql('DROP SCHEMA IF EXISTS %s CASCADE' % c.schema())
            BUILT.discard((c.dataset, rows))
        else:
            drop_added(c.schema())
    elif c.share is None and scaled or c.share and left[c.schema()] == 0:
        psql('DROP SCHEMA IF EXISTS %s CASCADE; DROP SCHEMA IF EXISTS %s_base CASCADE' % (c.schema(), c.schema()))
        BUILT.discard((c.schema(), rows))


def gap_verdict(case, lane, error, judged):
    """'known' where the flagged lane lost its plan (a gap the run reports and does not fail), 'closed' where it kept it (the flag must go); None for any other lane or error, and where no plan is asserted (`judged` false: scale points, --any-plan)"""
    if not case.gap or case.gap[0] != lane or not judged:
        return None
    return 'closed' if error is None else 'known' if error.startswith('PLAN') else None


def settle(case, tag, lanes, errors, judged, failed, known, counts):
    """the failures and the known gaps of one case: an error of a lane is a failure when `counts(lane, error)`, a flagged lane is a known gap while it loses its plan and a failure once it keeps it"""
    for lane in lanes:
        err, verdict = errors.get(lane), gap_verdict(case, lane, errors.get(lane), judged)
        if verdict == 'closed':
            failed.append('%s%s on %s: gap closed: remove the `-- gap:` line of bench/cases/%s.sql (it named %s)' % (case.id, tag, lane, case.id, case.gap[1]))
        elif verdict == 'known':
            known.append('%s%s on %s: known gap (%s): closes with %s; %s' % (case.id, tag, lane, lane, case.gap[1], err))
        elif err and counts(lane, err):
            failed.append('%s%s on %s: %s' % (case.id, tag, lane, err))


def run_point(args, cases, rows, engines, results, failed, known):
    """every case at ROWS orders: the rows of `results` (a case id with its size when the run has scales), the failures in `failed`; the scale points keep the planner's plan"""
    scaled = bool(args.scales)
    tag = '@%d' % rows if scaled else ''
    runcfg.ROWS = rows
    t0 = time.time()
    csv = dataset(rows) if args.duck_port or any(c.dataset not in DATASETS for c in cases) else ''   # the orders: only the cases on them, the load cases and pg_duckdb read the CSV
    TIMES['(dataset)' + tag] = [round(time.time() - t0, 1), 0]
    if args.duck_port:
        t0 = time.time()
        load_duck(args.duck_port, csv)
        TIMES['(pg_duckdb load)' + tag] = [round(time.time() - t0, 1), 0]
    point = []
    left = {}   # the cases still to read each shared table or dataset schema: it is dropped after the last, so a cluster holds one at a time
    for c in cases:
        if c.share or c.dataset in DATASETS:
            left[c.schema()] = left.get(c.schema(), 0) + 1
    for c in cases:
        log('%s%s: %s' % (c.id, ' at %s rows' % format(rows, ',') if scaled else '', c.label))
        t0 = time.time()
        build(c, rows)
        built = time.time() - t0
        if not args.no_wait:
            wait_quiet(args.wait)
        noisy = busy()
        load = server_load()
        if c.load:
            t1 = time.time()
            out, plans, errors = measure_load(c, csv, args.rounds, rows, args.minstr, args.profile)
            TIMES[c.id + tag] = [round(built, 1), round(time.time() - t1, 1)]
            settle(c, tag, out, errors, not scaled and not args.any_plan, failed, known, lambda lane, e: True)
            for lane, ms in out.items():
                for metric, xs in ms.items():
                    if not xs or args.io == 'off' and metric.startswith('io_'):   # no pg_stat_io on this server, or --io off
                        continue
                    point.append(dict(case=c.id, arm=lane, metric=metric, note=(('sift_table, segments written: %s' if 'USING {am}' in c.sec['prepare'] else 'BM25 index, segments: %s' if c.dataset == 'docs' else 'heap with a sift_col index, rows: %s') % plans[lane] if lane == 'sift' else 'heap' if lane == 'native' else 'sift_table, %s' % lane), load=load, **stats(xs)))
            release(c, left, rows, scaled)
            continue
        lanes = lane_sessions(c, engines, args.duck_port)
        t1 = time.time()
        times, notes, errors, texts = measure_query(c, lanes, args.rounds, args.runs, keep_plan=not (scaled or args.any_plan))
        if args.records:
            recs.emit(c, lanes, rows, args.runs)
        TIMES[c.id + tag] = [round(built, 1), round(time.time() - t1, 1)]
        settle(c, tag, lanes, errors, not scaled and not args.any_plan, failed, known, lambda lane, e: lane in ('sift', 'native') or e.startswith(('PLAN', 'RESULT')))
        for lane in lanes:
            if lane in errors:
                point.append(dict(case=c.id, arm=lane, metric='ms', value='NA' if errors[lane].startswith('not supported') else 'error', note=errors[lane], load=load))
            else:
                if 'ms' in c.metrics:
                    point.append(dict(case=c.id, arm=lane, metric='ms', note=notes[lane] + (' NOISY' if noisy else '') + (' [%s cpus]' % os.environ.get('CASES_CPUS', '?') if c.cpus_needed() > 1 else ''), load=load, **stats(times[lane])))
                if ('minstr' in c.metrics or args.minstr) and (lane in ('sift', 'native', 'base', 'btree') or 'minstr' in c.metrics) and lane != 'pg_duckdb' and not isinstance(lanes[lane], Fresh):   # a variant has instructions only where the case asks for the metric
                    try:
                        mi = minstr(lanes[lane], expand(c.statement(lane), c))
                    except PerfFailed as e:
                        failed.append('%s%s on %s: %s' % (c.id, tag, lane, e))
                    else:
                        point.append(dict(case=c.id, arm=lane, metric='minstr', value='%.1f' % mi, note=notes[lane], load=load))
                if lane != 'pg_duckdb' and not c.load:
                    ios, names, size = io_load(lanes[lane], c, lane, expand(c.statement(lane), c), texts.get(lane, ''), args.io)
                    for io in ios if args.io != 'off' else []:   # off: the rows of before the IO measure (buffers, index_bytes) only
                        for metric, value in iolib.metrics(io).items():
                            if metric.endswith('_cpu_ms') and os.environ.get('CASES_WORKERS', '2') != '0':   # the backend's CPU is the statement's only without parallel workers
                                continue
                            point.append(dict(case=c.id, arm=lane, metric=metric, value=value, note=io['how'] or 'warm', load=load))
                    if ios:
                        warm = ios[-1]
                        point.append(dict(case=c.id, arm=lane, metric='buffers', value=str(warm['hit'] + warm['read']), note='shared hit + read of one run, read=%d' % warm['read'], load=load))
                    if names:
                        point.append(dict(case=c.id, arm=lane, metric='index_bytes', value=str(size), note=', '.join(names), load=load))
            lanes[lane].close()
        if 'why' in notes:
            point.append(dict(case=c.id, arm='why', metric='ms', value='', note=notes['why'], load=load))
        release(c, left, rows, scaled)
    print(('rows %s\n' % format(rows, ',') if scaled else '') + summarize(point, cases))
    results += [dict(r, case=r['case'] + tag) for r in point]


def cmd_run(args):
    runcfg.LANE_BUDGET = args.lane_budget
    cases = [c for c in read_registry() if not args.cases or c.id in args.cases]
    if not cases:
        die('no case matches %s' % ','.join(args.cases))
    if args.no_share:
        for c in cases:
            c.share = None
    runcfg.TIMEOUT = args.timeout
    engines = args.engines.split(',')
    results, failed, known = [], [], []
    for scale in [int(x) for x in args.scales.split(',')] if args.scales else [1]:
        run_point(args, cases, args.rows * scale, engines, results, failed, known)
    log('time: %.0f s of builds (the dataset and the copy in pg_duckdb included), %.0f s of timed runs, %d cases, %d tables built for them' % (
        sum(b for b, _ in TIMES.values()), sum(r for _, r in TIMES.values()), len(cases), len({c.schema() for c in cases if c.dataset not in DATASETS})))
    for k in known:
        print('KNOWN GAP ' + k)
    if known:
        print('%d known gap%s (exit status unchanged): a flagged case that keeps its plan fails the run until its `-- gap:` line is removed' % (len(known), '' if len(known) == 1 else 's'))
    for f in failed:
        print('FAILED ' + f, file=sys.stderr)
    write_results(args, cases, results)
    if args.records:
        recs.write(args.records)
    if args.budget:
        from . import budget
        if args.budget_update:
            budget.update(args.budget, results, '# case<TAB>ceiling: millions of user instructions of one statement of the sift lane at 300,000 orders, the recorded count + 15% or 0.5 (bench/bench cases --minstr --rows 300000 --budget FILE --budget-update)\n')
        lines, ok = budget.check(args.budget, results)
        print('\n'.join(lines))
        failed += [] if ok else ['a case is over its instruction ceiling (%s)' % args.budget]
    if args.docs and not failed:   # the blocks that quote bench/results/cases.tsv and cases-scale.tsv
        import cases_render
        cases_render.render(os.path.join(REPO, 'bench', 'results'), os.path.join(REPO, 'bench', 'out', 'README-blocks.md'), os.path.join(REPO, 'bench', 'out', 'BENCHMARKS.md'))
        subprocess.run([sys.executable, '-I', os.path.join(HERE, 'highlights.py')], check=True, stdout=subprocess.DEVNULL)
    return 1 if failed else 0


def cmd_plan(args):
    """what a run would do, no server: the points, the builds, the disk and the time it needs (estimates fitted to the 1,000,000-row run of 2026-10-24, 86 cases in 17 min with shared tables: linear in the rows, a floor on a loaded host)"""
    cases = [c for c in read_registry() if not args.cases or c.id in args.cases]
    scales = [int(x) for x in args.scales.split(',')] if args.scales else [1]
    events = sum(c.dataset in DATASETS for c in cases)
    tmp = os.environ.get('CASES_TMP', '/dev/shm/sift-cases')
    probe = tmp
    while not os.path.exists(probe):
        probe = os.path.dirname(probe)
    free = os.statvfs(probe).f_bavail * os.statvfs(probe).f_frsize / 2**30
    seconds, peak = 0.0, 0.0
    print('cases plan: %d cases (%d on orders, %d on shared datasets) at %d point%s of %s orders, %d rounds of %d timed runs, statement timeout %d s' % (
        len(cases), len(cases) - events, events, len(scales), 's' * (len(scales) > 1), ' x '.join([format(args.rows, ','), str(scales)]), args.rounds, args.runs, args.timeout))
    for sc in scales:
        n = args.rows * sc
        t = n * 25e-6 + (n * 50e-6 if events else 0) + len(cases) * (n * 8e-6 + 4.0 * sc)
        gb = (n * (120 + 85 + 350) + (n * 1000 if events else 0)) / 2**30 + 4
        seconds, peak = seconds + t, max(peak, gb)
        print('  %14s orders (%dx): generate%s, %d builds and measures, about %4.0f min, up to %4.0f GB of scratch' % (format(n, ','), sc, ' orders and events' if events else ' orders', len(cases), t / 60, gb))
    print('  total about %.1f h; the largest point needs about %.0f GB under CASES_TMP=%s (%.0f GB free there); memory cap CASES_MEM=%s' % (seconds / 3600, peak, tmp, free, os.environ.get('CASES_MEM', '8G')))
    print('  the schema of each orders case is dropped after its measure; the events schema (one for the dataset) stays until the next size')
    return 0 if free > peak else 1


def write_results(args, cases, results):
    import extras
    version = re.search(r"default_version\s*=\s*'([^']*)'", open(os.path.join(REPO, 'pg_sift.control')).read()).group(1)
    sha = subprocess.run(['git', '-C', REPO, 'rev-parse', '--short=12', 'HEAD'], capture_output=True, text=True).stdout.strip()
    meta = {'orders': args.rows, 'postgresql': psql('SHOW server_version', 'postgres'), 'pg_sift': version, 'build': sha, 'date': datetime.date.today().isoformat(),
            'cpu': host(), 'method': 'client-side milliseconds of one psql session per lane, interleaved: %d rounds of 1 untimed and %d timed runs, median of all; the sift lane is the '
            'planner\'s own choice, native is sift.enable_agg = off on the same table; %s parallel workers under a quota of %s cpus; every answer compared with native' % (args.rounds, args.runs, os.environ.get('CASES_WORKERS', '2'), os.environ.get('CASES_CPUS', '?')),
            'work_mem': os.environ.get('CASES_WORK_MEM', '64MB')}
    if IO_HOW:
        meta['io_cold'] = '; '.join(sorted(IO_HOW)) + ' (the cold run is the first after the relations of the case were emptied; io_warm is the run after it; steps/iolib.py)'
    for r in results:
        r['version'] = version
    path = args.out
    if args.docs:
        path = os.path.join(REPO, 'bench', 'results', 'cases-scale.tsv' if args.scales else 'cases.tsv')
        if os.path.exists(path):   # a run of some cases replaces only their rows, at the size of the others
            old_meta, old = extras.read(path)
            ran = {r['case'] for r in results}
            if not args.scales and str(args.rows) != old_meta.get('orders') and {c.id for c in cases} != {c.id for c in read_registry()}:
                die('bench/results/cases.tsv holds %s orders: run every case at --rows %d, or these at --rows %s' % (old_meta.get('orders'), args.rows, old_meta.get('orders')))
            if args.scales and str(args.rows) != old_meta.get('orders'):
                die('bench/results/cases-scale.tsv has its 1x at %s orders: run with --rows %s' % (old_meta.get('orders'), old_meta.get('orders')))
            results = [r for r in old if r['case'] not in ran and str(args.rows) == old_meta.get('orders')] + results   # a run of every case at another size replaces the file
    if args.scales:
        meta['scales'] = args.scales
    extras.write(path, 'Benchmark cases (bench/cases.tsv)%s: pg_sift against PostgreSQL and the other engines on the orders of the orders benchmark.' % (' at 1x, 10x and 100x of --rows (the case id carries the number of orders: ID@ROWS)' if args.scales else ''), meta, results)
    log('wrote %s' % path)
    if args.docs or args.times:
        write_times(args, cases)


def write_times(args, cases):
    """bench/results/cases-time.tsv: the seconds each case took (its build: the table, indexes and setup, zero for a case that reads a table another one built; its timed runs on every lane),
    which the budget table of bench/README.md is made from; the rows of the cases that ran replace theirs"""
    path = args.times or os.path.join(REPO, 'bench', 'results', 'cases-time-scale.tsv' if args.scales else 'cases-time.tsv')
    old = {}
    if os.path.exists(path):
        for l in open(path):
            f = l.rstrip('\n').split('\t')
            if not l.startswith('#') and f[0] != 'case' and len(f) == 4:
                old[f[0]] = f
    for k, (b, r) in TIMES.items():
        old[k] = [k, str(args.rows), str(b), str(r)]
    lines = ['# Seconds of each case of bench/cases.tsv (steps/cases.py, bench/bench cases --docs): build (the table, its indexes and setup; 0 for a case that reads the table of an earlier one), run (the timed runs of every lane,',
             '# %d rounds of %d runs, a lane stops repeating after %g s), at the orders of the row. (dataset): generating the orders; (pg_duckdb load): the copy in the container.' % (args.rounds, args.runs, args.lane_budget),
             '# measured %s on %s.' % (datetime.date.today().isoformat(), host()),
             'case\torders\tbuild_s\trun_s'] + ['\t'.join(v) for v in old.values()]
    open(path, 'w').write('\n'.join(lines) + '\n')
    log('wrote %s' % path)


def cmd_cpus(args):
    """the quota the selected cases need (steps/cases.sh starts the cluster under it): one line, `CPUS WHO`"""
    if args.selftest:
        return selftest_cpus()
    cases = [c for c in read_registry() if not args.cases or c.id in args.cases]
    need, who = lane_cpus(cases, args.cpus)
    print(need, who or '-')


def selftest_cpus():
    class C:
        def __init__(self, id, variants):
            self.id, self.variants = id, variants
        cpus_needed = Case.cpus_needed

    w = lambda n: ['workers_%d' % n, 'max_parallel_maintenance_workers = %d' % n]
    cases = [C('plain', []), C('scan', [['scan', 'sift.agg_preagg=off'], ['g3', 'max_parallel_workers_per_gather = 3']]), C('build', [w(0), w(4), w(8)])]
    ok = [lane_cpus(cases[:1], 1) == (1, None), lane_cpus(cases[:2], 1) == (4, 'scan'), lane_cpus(cases, 1) == (9, 'build'), lane_cpus(cases, 16) == (16, None),
          workers_asked('max_parallel_maintenance_workers=2; x = 1') == 2, workers_asked('sift.agg_preagg=off') == 0]
    if not all(ok):
        print('cases cpus selftest failed: %s' % ok)
        return 1
    print('cases cpus selftest ok')
    return 0


def cmd_list(_):
    for c in read_registry():
        print('%-26s %-9s %-30s %s' % (c.id, c.dataset, ','.join(['sift', 'native'] + c.baselines[1:] if c.baselines[:1] == ['native'] else c.baselines), c.label))
