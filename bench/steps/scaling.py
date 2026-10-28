#!/usr/bin/env python3
"""Aggregates by number of partitions and parallel workers: the orders of the orders benchmark on a scratch cluster (bench/steps/scaling.sh runs it).

    scaling.py run --db DB --out FILE [--rows N] [--partitions 1,16,...] [--workers 0,1,2,3] [--reps 7] [--meta KEY=VALUE ...]

The orders table (bench/sql/store/gen.sql, `orders_src`) is split by a hash of the id into N partitions (N = 1 is the plain table) with a primary key and one sift_col index
(the layout of bench/sql/store/sift.sql); the questions below are timed on it with sift.enable_agg off (what PostgreSQL does) and on, one client, `psql \\timing` of one
statement with its planning, in random order interleaved over the cells, one untimed run first. Every pg_sift result is compared with the normal plan's. `partitions` runs
every N with 2 parallel workers; `workers` runs the plain table with max_parallel_workers_per_gather 0 .. W. The timing holds PERF_LOCK (default /tmp/sift-perf.lock) per
group of cells. Rows go to bench/results/scaling.tsv (columns of extras.py): case `partitions N` or `workers W`, arm `<question>_<pg|sift>`, metric median_ms.
"""
import argparse
import fcntl
import os
import random
import re
import statistics
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import pgq  # noqa: E402  (steps/pgq.py)
import extras  # noqa: E402

BENCH = os.path.dirname(HERE)
LOCK = os.environ.get('PERF_LOCK', '/tmp/sift-perf.lock')
QUESTIONS = ('point-lookup', 'revenue-per-month', 'top-categories', 'customers-per-month')   # a lookup by id (pruned to one partition), revenue per month, the top categories, customers per month
INDEX = "CREATE INDEX orders_col ON orders USING sift_col (customer_id, country, category, status, amount, quantity, created_at, (date_trunc('month', created_at)::date))"


def sql_of(q, rows):
    """the PostgreSQL statement of question q: the `--@ pg` section of its file in bench/sql/store/queries (the lookup asks for the order 42% of the way down)"""
    text = open(os.path.join(BENCH, 'sql', 'store', 'queries', q + '.sql')).read()
    return re.search(r'--@ pg\n(.*?)(?=\n--@|\Z)', text, re.S).group(1).strip().rstrip(';').replace('{point_id}', str(rows * 42 // 100))


def psql(db, sqls, timed=True, timeout=1800):
    """the statements run in one session, the last one timed: (ms, stdout)"""
    return pgq.timed(db, sqls, ['set jit = off', 'set statement_timeout = 0'], timed, timeout)


def build(db, parts):
    """orders as `parts` hash partitions of the id (1: plain), loaded from orders_src, with the primary key, the sift_col index, vacuumed and analyzed"""
    cols = 'id bigint NOT NULL, customer_id int NOT NULL, country text NOT NULL, category text NOT NULL, status text NOT NULL, amount numeric(10,2) NOT NULL, quantity smallint NOT NULL, created_at timestamp NOT NULL, title text NOT NULL'
    psql(db, ['DROP TABLE IF EXISTS orders CASCADE', f'CREATE TABLE orders ({cols})' + (' PARTITION BY HASH (id)' if parts > 1 else '')], False)
    if parts > 1:
        psql(db, [f"SELECT format('CREATE TABLE orders_p%s PARTITION OF orders FOR VALUES WITH (MODULUS {parts}, REMAINDER %s)', i, i) FROM generate_series(0, {parts - 1}) i \\gexec"], False)
    psql(db, ['INSERT INTO orders SELECT id, customer_id, country, category, status, amount, quantity, created_at, title FROM orders_src ORDER BY id', 'ALTER TABLE orders ADD PRIMARY KEY (id)', INDEX,
              'VACUUM (ANALYZE) orders'], False)


def plan_of(db, gucs, stmt):
    text = psql(db, gucs + ['EXPLAIN (COSTS OFF) ' + stmt], False)[1]
    return 'SiftAgg' if 'SiftAgg' in text else 'Index Scan' if 'Index Scan' in text else 'Seq Scan' if 'Seq Scan' in text else 'other'


def cells(case, workers, rows):
    """[(case, arm, gucs, statement)]: the two arms of every question at one setting"""
    par = f'set max_parallel_workers_per_gather = {workers}'
    return [(case, f'{q}_{sid}', [par, f'set sift.enable_agg = {"off" if sid == "pg" else "on"}'], sql_of(q, rows)) for q in QUESTIONS for sid in ('pg', 'sift')]


def time_cells(db, cs, reps, rows):
    plans = {(c[0], c[1]): plan_of(db, c[2], c[3]) for c in cs}
    ref = {}
    for c in cs:   # the untimed run of each cell; the result of the normal plan (the first arm of a question) is the reference
        ref.setdefault(c[1][:3], sorted(psql(db, c[2] + [c[3]], False)[1].splitlines()))
    times, ok, loads = {(c[0], c[1]): [] for c in cs}, {(c[0], c[1]): True for c in cs}, {(c[0], c[1]): [] for c in cs}
    with open(LOCK, 'a') as lock:
        print('scaling: waiting for %s' % LOCK, file=sys.stderr)
        fcntl.flock(lock, fcntl.LOCK_EX)
        print('scaling: timing %d cells x %d reps, %s' % (len(cs), reps, extras.host_load()), file=sys.stderr)
        for rep in range(reps + 1):
            order = cs[:]
            random.Random(rep).shuffle(order)
            for case, arm, gucs, stmt in order:
                ms, out = psql(db, gucs + [stmt])
                loads[(case, arm)].append(float(open('/proc/loadavg').read().split()[0]))
                if rep:
                    times[(case, arm)].append(ms)
                ok[(case, arm)] &= sorted(out.splitlines()) == ref[arm[:3]]
    for case, arm, _, _ in cs:
        t = times[(case, arm)]
        note = plans[(case, arm)] + ('' if ok[(case, arm)] else ' RESULT DIFFERS')
        rows.append(dict(case=case, arm=arm, metric='median_ms', value='%.2f' % statistics.median(t), min='%.2f' % min(t), max='%.2f' % max(t), n=str(len(t)),
                         load='%.2f' % statistics.mean(loads[(case, arm)]), note=note))
        print('%-16s %-10s %9s ms  %s' % (case, arm, rows[-1]['value'], note), file=sys.stderr)


def run(a):
    db = a.db
    version = psql(db, ['select sift.version()'], False)[1].strip()
    rows = []
    for parts in [int(x) for x in a.partitions.split(',') if x]:
        print('scaling: %d partitions' % parts, file=sys.stderr)
        build(db, parts)
        time_cells(db, cells(f'partitions {parts}', 2, a.rows), a.reps, rows)
    if a.workers:
        build(db, 1)
        for w in [int(x) for x in a.workers.split(',') if x]:
            time_cells(db, cells(f'workers {w}', w, a.rows), a.reps, rows)
    meta = dict(m.split('=', 1) for m in a.meta or [])
    meta.update({'bench': 'scaling', 'sift': version, 'orders': a.rows, 'host_load_after': extras.host_load(),
                 'method': 'psql \\timing of one statement, planning included, one client, %d timed runs after one untimed, cells interleaved in random order; jit off; partitions: 2 workers' % a.reps})
    for r in rows:
        r['version'] = version
    extras.write(a.out, 'Aggregates by number of partitions and parallel workers on the orders of the orders benchmark (bench/steps/scaling.sh).', meta, rows)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    sub = ap.add_subparsers(dest='cmd', required=True)
    r = sub.add_parser('run')
    r.add_argument('--db', default='scaling')
    r.add_argument('--out', required=True)
    r.add_argument('--rows', type=int, default=1000000)
    r.add_argument('--partitions', default='1,4,16,64,256,1024,4096')
    r.add_argument('--workers', default='0,1,2,3')
    r.add_argument('--reps', type=int, default=7)
    r.add_argument('--meta', action='append')
    a = ap.parse_args()
    run(a)


if __name__ == '__main__':
    main()
