#!/usr/bin/env python3
"""The planning matrix: every shape of bench/sql/plan_matrix/shapes.tsv on the tables of schema.sql, at several sizes and work_mem settings.

    python3 -I plan_matrix.py [--rows 100000,300000,1000000] [--work-mem 4MB,4GB] [--shapes REGEX] [--out FILE] [--no-planning] [--strict]
                              [--workers N] [--setup-cost C]

Needs a server (PGHOST / PGPORT, `scripts/pgtest.sh env`) with pg_sift installed; one database plan_matrix_<rows> per size, built once. For each
shape, size and work_mem, one backend, no workers (--workers N allows N per Gather, --setup-cost sets parallel_setup_cost), no JIT, the planner's own choice with the extension on against sift.enable_agg = off:

    sift         the plan has a Custom Scan (SiftAgg)
    same_as_off  when the scan is not chosen, the plan is the one without the extension (the extension must not change a plan it does not take)
    est_x        planned rows of the first node under the Limit against the rows it returned, as a factor (>= 1; rows x loops); over 2 is a violation
    plan_instr   user instructions to plan the shape with the extension on against off (perf stat of the backend, 20 EXPLAINs each); over off + 5%
                 and 20,000 instructions is a violation; '-' without perf
    on_ms off_ms median of 5 runs, warm
    scan_ms      when the scan is not chosen, the median of 5 runs with it forced (sift.agg_force); '-' when it is the plan or forcing it changes nothing (the statement
                 has no scan to force: the two times of one plan differ by noise alone)
    slower       the plan the planner chose runs over 5% longer than the other (the plan without the extension when the scan is chosen, the scan forced when it is not)

A violation is printed after the table; --strict makes any of them exit 1. Rows go to --out as TSV.
"""
import argparse, json, os, re, signal, statistics, subprocess, sys, time

HERE = os.path.dirname(os.path.abspath(__file__))
SQL = os.path.join(HERE, '..', 'sql', 'plan_matrix')
WORKERS, SETUP_COST = 0, None
SLOWER_BAND = 1.05


def settings(wm, mode):
    """The settings of a run: mode on (the planner's choice), off (the extension off) or force (the scan, whatever the cost)."""
    s = "SET max_parallel_workers_per_gather = %d; SET jit = off; SET work_mem = '%s'; SET sift.enable_agg = %s; SET sift.agg_force = %s;" % (
        WORKERS, wm, 'off' if mode == 'off' else 'on', 'on' if mode == 'force' else 'off')
    return s + (" SET parallel_setup_cost = %s;" % SETUP_COST if SETUP_COST is not None else '')
PLAN_RUNS, PLAN_FLOOR, EST_BAND, PLAN_BAND = 20, 20000, 2.0, 1.05


def psql(db, sql, *flags):
    r = subprocess.run(['psql', '-X', '-qAt', '-v', 'ON_ERROR_STOP=1', *flags, '-d', db, '-c', sql], capture_output=True, text=True)
    if r.returncode:
        sys.exit('plan_matrix: %s\n%s' % (sql[:200], r.stderr.strip()))
    return r.stdout


def build(rows):
    db = 'plan_matrix_%d' % rows
    if psql('postgres', "SELECT 1 FROM pg_database WHERE datname = '%s'" % db).strip():
        return db
    psql('postgres', 'CREATE DATABASE ' + db)
    r = subprocess.run(['psql', '-X', '-q', '-d', db, '-v', 'n=%d' % rows, '-f', os.path.join(SQL, 'schema.sql')], capture_output=True, text=True)
    if r.returncode:
        psql('postgres', 'DROP DATABASE ' + db)
        sys.exit('plan_matrix: schema.sql at %d rows\n%s' % (rows, r.stderr.strip()))
    return db


def shapes(only):
    for line in open(os.path.join(SQL, 'shapes.tsv')):
        if line.startswith('#') or not line.strip():
            continue
        name, cls, stmt = line.rstrip('\n').split('\t')
        if not only or re.search(only, name):
            yield name, cls, stmt


def explain(db, wm, mode, stmt, analyze=False):
    opts = '(ANALYZE, TIMING OFF, FORMAT JSON)' if analyze else '(COSTS OFF)'
    out = psql(db, settings(wm, mode) + ' EXPLAIN ' + opts + ' ' + stmt)
    return json.loads(out)[0]['Plan'] if analyze else re.sub(r'\s+', ' ', out)


def est_factor(plan):
    while plan['Node Type'] == 'Limit' and plan.get('Plans'):
        plan = plan['Plans'][0]
    est, got = max(plan['Plan Rows'], 1), max(plan['Actual Rows'] * plan.get('Actual Loops', 1), 1)
    return max(est / got, got / est)


def median_ms(db, wm, mode, stmt):
    ms = []
    for _ in range(5):
        out = psql(db, settings(wm, mode) + ' EXPLAIN (ANALYZE, TIMING OFF, SUMMARY ON) ' + stmt)
        ms.append(float(re.search(r'Execution Time: ([\d.]+)', out).group(1)))
    return statistics.median(ms)


def instructions(db, wm, mode, stmt):
    """User instructions of the backend over PLAN_RUNS EXPLAINs, after one to warm the caches; None without perf."""
    p = subprocess.Popen(['psql', '-X', '-qAt', '-d', db], stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True)
    p.stdin.write('SELECT pg_backend_pid();\n' + settings(wm, mode) + '\n\\o /dev/null\nEXPLAIN ' + stmt + ';\n')
    p.stdin.flush()
    pid = p.stdout.readline().strip()
    out = '/tmp/plan_matrix_perf.%d' % os.getpid()
    try:
        perf = subprocess.Popen(['perf', 'stat', '-x,', '-e', 'instructions:u', '-p', pid, '-o', out], stderr=subprocess.DEVNULL)
    except FileNotFoundError:
        p.kill()
        return None
    time.sleep(0.3)
    p.stdin.write(('EXPLAIN ' + stmt + ';\n') * PLAN_RUNS + '\\o\nSELECT 1;\n')
    p.stdin.close()
    p.wait()
    perf.send_signal(signal.SIGINT)
    perf.wait()
    m = re.search(r'^(\d+),,instructions', open(out).read(), re.M) if os.path.exists(out) else None
    if os.path.exists(out):
        os.remove(out)
    return int(m.group(1)) / PLAN_RUNS if m else None


def cell(db, wm, name, cls, stmt, planning):
    plan_on, plan_off = explain(db, wm, 'on', stmt), explain(db, wm, 'off', stmt)
    sift = 'SiftAgg' in plan_on
    row = {'shape': name, 'class': cls, 'rows': db.rsplit('_', 1)[1], 'work_mem': wm, 'sift': sift, 'same_as_off': '-' if sift else plan_on == plan_off,
           'est_x': round(est_factor(explain(db, wm, 'on', stmt, True)), 1), 'on_ms': median_ms(db, wm, 'on', stmt), 'off_ms': median_ms(db, wm, 'off', stmt)}
    forced = not sift and explain(db, wm, 'force', stmt) != plan_on
    row['scan_ms'] = median_ms(db, wm, 'force', stmt) if forced else '-'
    other = row['off_ms'] if sift else row['scan_ms']
    row['slower'] = (sift or forced) and row['on_ms'] > other * SLOWER_BAND and row['on_ms'] - other > 0.5
    on = off = None
    if planning:
        on, off = instructions(db, wm, 'on', stmt), instructions(db, wm, 'off', stmt)
    row['plan_on_k'], row['plan_off_k'] = ('-', '-') if on is None or off is None else (round(on / 1000), round(off / 1000))
    return row


def violations(r):
    if r['same_as_off'] is False:
        yield 'the plan without the scan differs from the plan with the extension off'
    if r['est_x'] > EST_BAND:
        yield 'rows planned and returned differ by %.1fx' % r['est_x']
    if r['plan_on_k'] != '-' and r['plan_on_k'] * 1000 > r['plan_off_k'] * 1000 * PLAN_BAND + PLAN_FLOOR:
        yield 'planning takes %dk instructions against %dk' % (r['plan_on_k'], r['plan_off_k'])


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--rows', default='100000,300000,1000000')
    ap.add_argument('--work-mem', default='4MB,4GB')
    ap.add_argument('--shapes')
    ap.add_argument('--out')
    ap.add_argument('--no-planning', action='store_true')
    ap.add_argument('--strict', action='store_true')
    ap.add_argument('--workers', type=int, default=0)
    ap.add_argument('--setup-cost', default=None)
    a = ap.parse_args()
    global WORKERS, SETUP_COST
    WORKERS, SETUP_COST = a.workers, a.setup_cost
    cols = ['shape', 'class', 'rows', 'work_mem', 'sift', 'same_as_off', 'est_x', 'plan_on_k', 'plan_off_k', 'on_ms', 'off_ms', 'scan_ms', 'slower']
    rows, bad = [], []
    for n in map(int, a.rows.split(',')):
        db = build(n)
        for wm in a.work_mem.split(','):
            for name, cls, stmt in shapes(a.shapes):
                r = cell(db, wm, name, cls, stmt, not a.no_planning)
                rows.append(r)
                bad += [(r, v) for v in violations(r)]
                print('\t'.join(str(r[c]) if not isinstance(r[c], float) else '%.2f' % r[c] for c in cols), flush=True)
    if a.out:
        with open(a.out, 'w') as f:
            f.write('\t'.join(cols) + '\n')
            f.writelines('\t'.join(str(r[c]) for c in cols) + '\n' for r in rows)
    print('\n%d cells, %d on the scan, %d chosen slower (over %d%% longer than the other plan), %d violations' % (
        len(rows), sum(r['sift'] for r in rows), sum(r['slower'] for r in rows), round((SLOWER_BAND - 1) * 100), len(bad)))
    for r, v in bad:
        print('  %s %s rows %s work_mem %s: %s' % (r['shape'], r['class'], r['rows'], r['work_mem'], v))
    sys.exit(1 if bad and a.strict else 0)


if __name__ == '__main__':
    main()
