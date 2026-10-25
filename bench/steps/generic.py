#!/usr/bin/env python3
"""The generic suites (TPC-H-like, ClickBench-like, time series: bench/cases.tsv datasets tpch, hits, tseries) on the other engines of bench/engines.tsv (kind analytic), with the answers checked
against PostgreSQL's before any time is taken. Standard library, psql and docker only.

    python3 -I generic.py run [--rows N] [--suites tpch,hits,tseries] [--engines ID,ID] [--runs 5] [--check-only] [--work DIR] [--out FILE] [--port P]

One engine at a time (a container, or a process for DuckDB itself), each an adapter bench/engines/ID.py over steps/suite_engine.py. The source (pg_duckdb: PostgreSQL 18 with pg_duckdb) comes first: it
generates each suite with bench/sql/SUITE/gen.sql (its pg_sift index lines left out), answers every query with PostgreSQL's own plan (the arm `pg`, the reference) and with
`duckdb.force_execution = on`, and exports the tables as CSV and their schema as JSON in --work. The other engines load that CSV and answer the same statements (a `--@ SECTION` of the case,
the engine's dialect in bench/engines.tsv, replaces the statement where the dialects differ). A case an engine does not run is a row with value NA and the reason in `note`, never a missing row.
Rows are `arm ms` of the warm median, like bench/results/cases.tsv; the pg_sift number of each query is the one of cases.tsv, so the comparison is made by steps/compare.py from the two files.
"""
import argparse
import os
import sys
import time
from types import SimpleNamespace

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import cases as cases_mod  # noqa: E402
import engines  # noqa: E402  (steps/engines.py: bench/engines.tsv)
from suite_engine import REPO, Unsupported, canon, first_diff, log, same, timed  # noqa: E402


def suite_cases(suites):
    return [c for c in cases_mod.read_registry() if c.dataset in suites]


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('cmd', choices=['run'])
    ap.add_argument('--rows', type=int, default=1000000)
    ap.add_argument('--suites', default='tpch,hits,tseries')
    ap.add_argument('--engines', help='ids of bench/engines.tsv (kind analytic); default: those with default = yes')
    ap.add_argument('--runs', type=int, default=5)
    ap.add_argument('--cpus', type=int, default=3)
    ap.add_argument('--memory', default='8g')
    ap.add_argument('--timeout', type=int, default=60)
    ap.add_argument('--check-only', action='store_true', help='answer every query once and compare; no times')
    ap.add_argument('--work', default=os.environ.get('GENERIC_WORK', '/tmp/sift-generic'))
    ap.add_argument('--out')
    ap.add_argument('--docs', action='store_true', help='write bench/results/generic.tsv')
    ap.add_argument('--port', type=int, default=56180)
    a = ap.parse_args()
    suites = a.suites.split(',')
    registry = engines.rows('analytic')
    chosen = a.engines.split(',') if a.engines else [r['id'] for r in registry if r['default'] == 'yes']
    unknown = [e for e in chosen if e not in [r['id'] for r in registry]]
    if unknown:
        sys.exit('generic: %s is not an analytic engine of bench/engines.tsv (%s)' % (','.join(unknown), ' '.join(r['id'] for r in registry)))
    adapters = {r['id']: engines.adapter(r['id']) for r in registry if r['id'] in chosen or getattr(engines.adapter(r['id']), 'SOURCE', False)}
    order = sorted(adapters, key=lambda e: (not getattr(adapters[e], 'SOURCE', False), [r['id'] for r in registry].index(e)))   # the source first, then the registry's order
    if a.docs:
        a.out = os.path.join(REPO, 'bench', 'results', 'generic.tsv')
    os.makedirs(a.work, exist_ok=True)
    ctx = SimpleNamespace(port=a.port, cpus=a.cpus, memory=a.memory, timeout=a.timeout, work=a.work)
    cases = suite_cases(suites)
    runs = 0 if a.check_only else a.runs
    results, failures, reference, skipped = [], [], {}, {}   # (case, arm, median, min, max, note); the PostgreSQL answer by case; engine -> [case: reason]

    for eid in order:
        eng = adapters[eid].Engine(ctx)
        log('%s (one %s), %d rows of each suite' % (eid, 'process' if eid == 'duckdb' else 'container', a.rows))
        eng.up()
        try:
            for suite in suites:
                if getattr(adapters[eid], 'SOURCE', False):
                    eng.generate(suite, a.rows, a.work)
                else:
                    eng.load(suite, a.work)
            for c in cases:
                for arm in eng.arms(c):
                    note, med, lo, hi = '', None, None, None
                    try:
                        sql = eng.statement(c, arm)
                        note = eng.precheck(arm, sql) or ''
                        if not note:
                            med, lo, hi, got = timed(eng.runner(arm, sql), runs)
                            got = canon(got)
                            if arm == 'pg':
                                reference[c.id] = got
                                if not got or got == [['']]:
                                    log('%s: the answer is empty at %d rows, so the engines agree on nothing' % (c.id, a.rows))
                            elif not same(reference[c.id], got):
                                note = 'answers differ from PostgreSQL (%d rows against %d)%s' % (len(got), len(reference[c.id]), first_diff(reference[c.id], got))
                                if eng.approximate(sql):
                                    note = '%s: count(DISTINCT) is an estimate in this engine' % note
                                else:
                                    failures.append('%s: %s %s' % (c.id, arm, note))
                    except Unsupported as u:
                        note = str(u)
                    if note:
                        med = lo = hi = None
                        skipped.setdefault(arm, []).append('%s: %s' % (c.id, note))
                    results.append((c.id, arm, med, lo, hi, open('/proc/loadavg').read().split()[0], note))
        finally:
            eng.down()

    ok = len({c.id for c in cases}) - len({f.split(':')[0] for f in failures})
    log('%d of %d queries answered alike by every engine that ran them%s' % (ok, len(cases), '' if not failures else '; ' + '; '.join(failures)))
    for arm in dict.fromkeys(r[1] for r in results):
        why = skipped.get(arm, [])
        total = sum(1 for r in results if r[1] == arm)
        loads = [float(r[5]) for r in results if r[1] == arm]
        log('%-17s %2d of %2d compared, host load %.1f to %.1f%s' % (arm, total - len(why), total, min(loads), max(loads), ', not run: ' + '; '.join(w[:150] for w in why) if why else ''))
    if a.out and runs:
        with open(a.out, 'w') as f:
            f.write('# The generic suites on the other engines (steps/generic.py, bench/engines.tsv): %s against PostgreSQL 18 (the pg_duckdb image), the same rows, answers compared before any time.\n' % ', '.join(order))
            f.write('# rows: %d\n# date: %s\n# method: client-side milliseconds, warm median of %d runs after one untimed; %d CPUs and %s per engine, one engine at a time; pg is PostgreSQL 18.1 of the pg_duckdb image\n'
                    % (a.rows, time.strftime('%Y-%m-%d'), runs, a.cpus, a.memory))
            f.write('# NA: the engine did not run the case, the reason is in note; load: the load average (1 minute) of the host when the case finished, so a noisy engine is reported, not skipped\n')
            f.write('version\tcase\tarm\tmetric\tvalue\tmin\tmax\tn\tload\tnote\n')
            for case, arm, med, lo, hi, load, note in results:
                f.write('\t'.join([str(x) for x in ('3.4', case, arm, 'ms', 'NA' if med is None else '%.3f' % med, '' if lo is None else '%.3f' % lo, '' if hi is None else '%.3f' % hi, runs, load, note)]) + '\n')
    sys.exit(1 if failures else 0)


if __name__ == '__main__':
    main()
