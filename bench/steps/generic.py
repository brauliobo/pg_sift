#!/usr/bin/env python3
"""The generic suites (TPC-H-like, ClickBench-like, time series: bench/cases.tsv datasets tpch, hits, tseries) on the other engines of bench/engines.tsv (kind analytic), with the answers checked
against PostgreSQL's before any time is taken. Standard library, psql and docker only.

    python3 -I generic.py run [--rows N] [--suites tpch,hits,tseries,orders] [--engines ID,ID] [--runs 5] [--check-only] [--work DIR] [--out FILE] [--port P]

One engine at a time (a container, or a process for DuckDB itself), each an adapter bench/engines/ID.py over steps/suite_engine.py. The source (pg_duckdb: PostgreSQL 18 with pg_duckdb) comes first: it
generates each suite with bench/sql/SUITE/gen.sql (its pg_sift index lines left out), answers every query with PostgreSQL's own plan (the arm `pg`, the reference) and with
`duckdb.force_execution = on`, and exports the tables as CSV and their schema as JSON in --work. The other engines load that CSV and answer the same statements (a `--@ SECTION` of the case,
the engine's dialect in bench/engines.tsv, replaces the statement where the dialects differ). A case an engine does not run is a row with value NA and the reason in `note`, never a missing row.
Rows are `arm ms` of the warm median, like bench/results/cases.tsv; the pg_sift number of each query is the one of cases.tsv, so the comparison is made by steps/compare.py from the two files.
"""
import argparse
import hashlib
import os
import subprocess
import sys
import time
from types import SimpleNamespace

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import cases as cases_mod  # noqa: E402
import engines  # noqa: E402  (steps/engines.py: bench/engines.tsv)
import benchlib  # noqa: E402  (steps/benchlib.py)
from suite_engine import REPO, Unsupported, canon, first_diff, log, same, timed  # noqa: E402

VERSION = next(l.split("'")[1] for l in open(os.path.join(REPO, 'pg_sift.control')) if l.startswith('default_version'))   # the rows carry the version they were measured at


def suite_cases(suites):
    """the cases of the suites; of the orders, the headline cases that only read the table (the others have a setup of their own or write)"""
    return [c for c in cases_mod.read_registry() if c.dataset in suites and (c.dataset != 'orders' or c.headline and not c.load and not {'setup', 'prepare'} & set(c.sec))]


def inputs_key(a, cases, chosen):
    """what the numbers of the other engines depend on: the harness, the adapters of the engines that run and the source, the generators and the statements of the cases, the rows and the runs.
    Not the pg_sift sources: this stage does not run pg_sift (its numbers are the cases stage's), so a commit that only changes pg_sift leaves the key alone"""
    root = lambda *p: os.path.join(REPO, 'bench', *p)
    files = [root('steps', f) for f in ('generic.py', 'suite_engine.py', 'engines.py')] + [root('engines.tsv'), root('versions.env')]
    files += [root('engines', e + '.py') for e in chosen if os.path.exists(root('engines', e + '.py'))] + [root('engines', 'pg_duckdb.py')]
    files += [root('sql', s, f) for s in a.suites.split(',') for f in ('gen.sql', 'ch.sql', 'citus.sql') if os.path.exists(root('sql', s, f))]
    files += [root('cases', c.id + '.sql') for c in cases]
    return benchlib.key_of({'files': [[os.path.relpath(f, REPO), hashlib.sha256(open(f, 'rb').read()).hexdigest()] for f in sorted(set(files)) if os.path.exists(f)],
                            'cases': [[c.id, c.dataset] for c in cases], 'rows': a.rows, 'runs': a.runs, 'cpus': a.cpus, 'memory': a.memory, 'engines': sorted(chosen)})


def host_words():
    """cpu, threads and memory of this host: what an older file must have for its numbers to stand in (the kernel and docker versions are not asked)"""
    fp = benchlib.host_fingerprint()
    return '%s, %s threads, %s' % (fp['cpu'], fp['threads'], fp['ram'])


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('cmd', choices=['run', 'key'], help='key: print the inputs key and the host of a run with these options, and whether bench/results/generic.tsv carries both (exit 0) or not (exit 1)')
    ap.add_argument('--rows', type=int, default=1000000)
    ap.add_argument('--suites', default='tpch,hits,tseries,orders')
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
    key, host = inputs_key(a, cases, chosen), host_words()
    if a.cmd == 'key':
        have = {l[2:].split(':', 1)[0]: l.split(':', 1)[1].strip() for l in open(os.path.join(REPO, 'bench', 'results', 'generic.tsv')) if l.startswith('# ') and ':' in l}
        print('inputs %s, host %s; the file has inputs %s, host %s, measured %s' % (key, host, have.get('inputs'), have.get('host'), have.get('date')))
        sys.exit(0 if have.get('inputs') == key and have.get('host') == host else 1)
    runs = 0 if a.check_only else a.runs
    results, failures, reference, skipped = [], [], {}, {}   # (case, arm, median, min, max, note); the PostgreSQL answer by case; engine -> [case: reason]

    def run_suite(eng, suite_cases_):
        for c in suite_cases_:
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
                        elif c.id not in reference:
                            note = 'PostgreSQL did not answer the case on this dataset, so there is nothing to compare with'
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

    for eid in order:
        eng = adapters[eid].Engine(ctx)
        log('%s (one %s), %d rows of each suite' % (eid, 'process' if eid == 'duckdb' else 'container', a.rows))
        eng.up()
        try:
            for suite in suites:   # one suite at a time in an engine: two suites name a table alike (the orders of TPC-H and of the store)
                if getattr(adapters[eid], 'SOURCE', False):
                    eng.generate(suite, a.rows, a.work)
                else:
                    eng.load(suite, a.work)
                run_suite(eng, [c for c in cases if c.dataset == suite])
                eng.unload(suite, a.work)
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
            f.write('# inputs: %s\n# host: %s\n# sha: %s\n' % (key, host, subprocess.run(['git', '-C', REPO, 'describe', '--tags', '--always'], capture_output=True, text=True).stdout.strip()))
            f.write('# rows: %d\n# date: %s\n# method: client-side milliseconds, warm median of %d runs after one untimed; %d CPUs and %s per engine, one engine at a time; pg is PostgreSQL 18.1 of the pg_duckdb image\n'
                    % (a.rows, time.strftime('%Y-%m-%d'), runs, a.cpus, a.memory))
            f.write('# NA: the engine did not run the case, the reason is in note; load: the load average (1 minute) of the host when the case finished, so a noisy engine is reported, not skipped\n')
            f.write('version\tcase\tarm\tmetric\tvalue\tmin\tmax\tn\tload\tnote\n')
            for case, arm, med, lo, hi, load, note in results:
                f.write('\t'.join([str(x) for x in (VERSION, case, arm, 'ms', 'NA' if med is None else '%.3f' % med, '' if lo is None else '%.3f' % lo, '' if hi is None else '%.3f' % hi, runs, load, note)]) + '\n')
    sys.exit(1 if failures else 0)


if __name__ == '__main__':
    main()
