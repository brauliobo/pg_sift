#!/usr/bin/env python3
"""The set-filter benchmark: a fact table filtered by a set of entity ids (bench/steps/sets.sh runs it on a scratch cluster).

    sets.py run --db DB --out FILE [--reps 9] [--ch CONTAINER] [--only REGEX] [--meta KEY=VALUE ...]
    sets.py render [--results bench/results/sets.tsv] [--benchmarks docs/BENCHMARKS.md]

`run` times every cell of the table below on the PostgreSQL named by PGHOST / PGPORT / PGUSER (and, with --ch, on a ClickHouse container), in random
order interleaved over the cells, one untimed run first, and writes the rows of bench/results/sets.tsv (columns in extras.py). The time is the wall
time of one statement as `psql \\timing` reports it (planning included), one client; ClickHouse's is the client's `--time`. Every PostgreSQL result
is compared with the normal plan's, ClickHouse's counts with them. The run holds /tmp/sift-perf.lock (blocking) while it times.

Arms (the set is the dim_b of one group of the membership table: 1k, 2k, 5k entity ids, and 10k, 100k as headroom; the query counts and averages the facts of the last
12 months by source, soft-deleted facts excluded):
  array_generic    dim_a_id = ANY($1::int4[]) with plan_cache_mode = force_generic_plan; above 2,000 ids also enable_indexscan and enable_bitmapscan off
  array_custom     the same statement planned with the array as a constant (plan_cache_mode = force_custom_plan), default costs
  literal_in       dim_a_id IN (1, 2, ...) as written, default costs
  literal_in_agg   the same with the page costs raised, so that Custom Scan (SiftAgg) is kept
  normal_plan      the same literal list with sift.enable_agg = off: what PostgreSQL does without sift
  member_in        dim_a_id IN (SELECT dim_a_id FROM memberships WHERE group_id = G AND deleted_at IS NULL), default costs
  member_in_agg    the same with the page costs raised
  member_in_agg_lut_off   the same with sift.agg_semi_lut = off (only where the setting exists)
  array_generic_T / normal_plan_T   the array parameter and the literal list on a copy of the facts whose entity key is of type T (int8, text, date), 1k and 5k
  ch_literal_ins / ch_literal_tuned / ch_member_tuned   ClickHouse, a literal list / IN (SELECT ...) on a table in the order of id / on one
                   partitioned by month and ordered by (foreign_id, dim_a_id, fact_date, id), max_threads 3
"""
import argparse
import datetime
import fcntl
import os
import random
import re
import statistics
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import extras  # noqa: E402
import pgq  # noqa: E402  (steps/pgq.py)

REPO = os.path.dirname(os.path.dirname(HERE))
LOCK = os.environ.get('PERF_LOCK', '/tmp/sift-perf.lock')
SIZES = {'1k': 1000, '2k': 2000, '5k': 5000, '10k': 10000, '100k': 100000}   # 1k to 5k is where the workload is, 10k and 100k are headroom
TYPE_SIZES = ('1k', '5k')
TYPES = {   # key type -> (table, array cast, key of entity id N in the text of a statement)
    'int8': ('facts_by_int8', 'int8[]', str),
    'text': ('facts_by_text', 'text[]', lambda n: 'ent-%07d' % n),
    'date': ('facts_by_date', 'date[]', lambda n: str(datetime.date(2000, 1, 1) + datetime.timedelta(days=n))),
}
WINDOW = "fact_date >= date '2025-10-01'"
SELECT = "SELECT foreign_id, count(*), round(avg(score)::numeric, 4) FROM {T} WHERE deleted_at IS NULL AND " + WINDOW + " AND dim_a_id {P} GROUP BY 1 ORDER BY 1"
COMMON = ['set max_parallel_workers_per_gather = 2', 'set jit = off']
FORCE = ['set seq_page_cost = 1000', 'set random_page_cost = 1000']   # keeps the SiftAgg plan when the normal plan would be picked
CH_WINDOW = "fact_date >= toDate('2025-10-01')"


def psql(db, sqls, timeout=600):
    """the statements run in one session, the last one timed: (ms, stdout)"""
    return pgq.timed(db, sqls, COMMON, timeout=timeout)


def groups(db):
    """size -> the first group of the membership table with that many entity ids"""
    rows = psql(db, ["select min(group_id), count(*) from memberships where deleted_at is null group by group_id having count(*) in (%s) order by 2, 1" % ','.join(map(str, SIZES.values()))])[1]
    first = {}
    for r in rows.strip().splitlines():
        g, n = r.split('|')
        first.setdefault(int(n), int(g))
    return {k: first[n] for k, n in SIZES.items()}


def ids(db, group):
    return [int(x) for x in psql(db, ['select dim_a_id from memberships where group_id = %d and deleted_at is null order by 1' % group])[1].split()]


def supported(db):
    """does sift_col take a key of this type: (type, error or '')"""
    out = []
    for t in ['int4', 'int8', 'text', 'date', 'uuid']:
        try:
            psql(db, ['drop table if exists probe', 'create table probe (k %s, v int)' % t, 'create index probe_col on probe using sift_col (k, v)', 'select 1'])
            out.append((t, ''))
        except RuntimeError as e:
            out.append((t, re.sub(r'\s+', ' ', re.sub(r'^.*?ERROR:\s*', '', str(e).split(' :: ')[0]).strip())[:200]))
    psql(db, ['drop table if exists probe', 'select 1'])
    return out


def ch(container, sql, threads=3):
    p = subprocess.run(['docker', 'exec', '-i', container, 'clickhouse-client', '--max_threads', str(threads), '--time', '--max_query_size', '100000000', '--format', 'TSV'],
                       input=sql, capture_output=True, text=True, timeout=600)
    if p.returncode:
        raise RuntimeError(p.stderr[:300] + sql[:120])
    return float(p.stderr.strip().splitlines()[-1]) * 1000, p.stdout


def cells(db, container, have_lut, group):
    """[(arm, size, kind, gucs, statement)]: kind 'pg' or 'ch'"""
    out = []
    for size, g in group.items():
        dim_b = ids(db, g)
        lst = ','.join(map(str, dim_b))
        sub = "IN (SELECT dim_a_id FROM memberships WHERE group_id = %d AND deleted_at IS NULL)" % g
        array_gucs = ['set plan_cache_mode = force_generic_plan'] + (['set enable_indexscan = off', 'set enable_bitmapscan = off'] if SIZES[size] > 2000 else [])
        sel = SELECT.format(T='facts', P='{}')
        pg = {
            'array_generic': (array_gucs, sel.format('= ANY($1::int4[])') + "\n\\bind '{%s}' \\g" % lst),
            'array_custom': (['set plan_cache_mode = force_custom_plan'], sel.format('= ANY($1::int4[])') + "\n\\bind '{%s}' \\g" % lst),
            'literal_in': ([], sel.format('IN (%s)' % lst)),
            'literal_in_agg': (FORCE, sel.format('IN (%s)' % lst)),
            'normal_plan': (['set sift.enable_agg = off'], sel.format('IN (%s)' % lst)),
            'member_in': ([], sel.format(sub)),
            'member_in_agg': (FORCE, sel.format(sub)),
        }
        if have_lut:
            pg['member_in_agg_lut_off'] = (FORCE + ['set sift.agg_semi_lut = off'], sel.format(sub))
        if size in TYPE_SIZES:
            for t, (table, cast, key) in TYPES.items():
                tsel = SELECT.format(T=table, P='{}')
                arr = ','.join(map(key, dim_b))
                lit = ','.join("'%s'" % key(m) for m in dim_b)
                pg['array_generic_' + t] = (array_gucs, tsel.format('= ANY($1::%s)' % cast) + "\n\\bind '{%s}' \\g" % arr)
                pg['normal_plan_' + t] = (['set sift.enable_agg = off'], tsel.format('IN (%s)' % lit))
        for arm, (gucs, stmt) in pg.items():
            out.append((arm, size, 'pg', gucs, stmt))
        if container:
            chsel = ("SELECT foreign_id, count(), round(avg(score), 4) FROM {t} WHERE deleted_at IS NULL AND " + CH_WINDOW + " AND dim_a_id IN ({P}) GROUP BY 1 ORDER BY 1")
            chsub = "SELECT dim_a_id FROM memberships WHERE group_id = %d AND deleted_at IS NULL" % g
            out.append(('ch_literal_ins', size, 'ch', [], chsel.format(t='facts_ins', P=lst)))
            out.append(('ch_literal_tuned', size, 'ch', [], chsel.format(t='facts_tuned', P=lst)))
            out.append(('ch_member_tuned', size, 'ch', [], chsel.format(t='facts_tuned', P=chsub)))
    return out


def plan_of(db, gucs, stmt):
    ex = 'EXPLAIN (COSTS OFF) ' + stmt
    try:
        text = psql(db, gucs + [ex])[1]
    except RuntimeError as e:
        return 'error: %s' % str(e)[:80]
    return 'SiftAgg' if 'SiftAgg' in text else 'Index Scan' if 'Index Scan' in text else 'Bitmap Heap Scan' if 'Bitmap Heap Scan' in text else 'Seq Scan' if 'Seq Scan' in text else 'other'


def same(pg_out, other, kind):
    if kind == 'pg':
        return pg_out == other
    a = [r.split('|') for r in pg_out.strip().splitlines()]
    b = [r.split('\t') for r in other.strip().splitlines()]
    return len(a) == len(b) and all(x[0] == y[0] and x[1] == y[1] for x, y in zip(a, b))


def run(a):
    db = a.db
    have_lut = psql(db, ["select count(*) from pg_settings where name = 'sift.agg_semi_lut'"])[1].strip() == '1'
    group = groups(db)
    cs = [c for c in cells(db, a.ch, have_lut, group) if not a.only or re.search(a.only, c[0] + ' ' + c[1])]
    version = psql(db, ["select extversion from pg_extension where extname = 'pg_sift'"])[1].strip()   # the library's own string can lag the extension
    plans = {(c[0], c[1]): plan_of(db, c[3], c[4]) if c[2] == 'pg' else 'ClickHouse' for c in cs}
    ref = {}
    for size in group:
        ref[size] = next(psql(db, c[3] + [c[4]])[1] for c in cells(db, None, have_lut, group) if c[0] == 'normal_plan' and c[1] == size)
    with open(LOCK, 'a') as lock:
        print('sets: waiting for %s' % LOCK, file=sys.stderr)
        fcntl.flock(lock, fcntl.LOCK_EX)
        load0 = extras.host_load()
        print('sets: timing %d cells x %d reps, %s' % (len(cs), a.reps, load0), file=sys.stderr)
        times = {(c[0], c[1]): [] for c in cs}
        loads = {(c[0], c[1]): [] for c in cs}
        ok = {(c[0], c[1]): True for c in cs}
        for rep in range(a.reps + 1):
            order = cs[:]
            random.Random(rep).shuffle(order)
            for arm, size, kind, gucs, stmt in order:
                ms, text = psql(db, gucs + [stmt]) if kind == 'pg' else ch(a.ch, stmt)
                loads[(arm, size)].append(float(open('/proc/loadavg').read().split()[0]))
                if rep:
                    times[(arm, size)].append(ms)
                ok[(arm, size)] &= same(ref[size], text, kind)
        load1 = extras.host_load()
    meta = dict(m.split('=', 1) for m in a.meta or [])
    meta.update({'bench': 'sets', 'sift': version, 'host_load_before': load0, 'host_load_after': load1,
                 'method': 'psql \\timing of one statement, planning included, one client, %d timed runs after one untimed, cells interleaved in random order; max_parallel_workers_per_gather 2, jit off' % a.reps})
    rows = [dict(version=version, case=t, arm='sift_col_key', metric='supported', value='0' if err else '1', min='', max='', n='', load='', note=err or 'ok') for t, err in supported(db)]
    for arm, size, kind, gucs, stmt in cs:
        t = times[(arm, size)]
        note = plans[(arm, size)] + ('' if ok[(arm, size)] else ' RESULT DIFFERS')
        rows.append(dict(version=version, case=size, arm=arm, metric='median_ms', value='%.2f' % statistics.median(t), min='%.2f' % min(t), max='%.2f' % max(t),
                         n=str(len(t)), load='%.2f' % statistics.mean(loads[(arm, size)]), note=note))
    extras.write(a.out, 'Set filters: a fact table filtered by 1k, 2k, 5k, 10k and 100k entity ids (bench/steps/sets.sh).', meta, rows)
    for r in rows:
        print('%-6s %-24s %9s ms  load %s  %s' % (r['case'], r['arm'], r['value'], r['load'], r['note']))


def render(a):
    meta, rows = extras.read(a.results)
    extras.replace_block(a.benchmarks, 'sets', extras.sets_section(meta, rows))


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    sub = ap.add_subparsers(dest='cmd', required=True)
    r = sub.add_parser('run')
    r.add_argument('--db', default='sets')
    r.add_argument('--out', required=True)
    r.add_argument('--reps', type=int, default=9)
    r.add_argument('--ch', default='', help='the name of a running ClickHouse container with the tables of sets.sh')
    r.add_argument('--only', default='', help='a regex over "arm size" of the cells to run')
    r.add_argument('--meta', action='append')
    g = sub.add_parser('render')
    g.add_argument('--results', default=os.path.join(REPO, 'bench', 'results', 'sets.tsv'))
    g.add_argument('--benchmarks', default=os.path.join(REPO, 'docs', 'BENCHMARKS.md'))
    a = ap.parse_args()
    run(a) if a.cmd == 'run' else render(a)


if __name__ == '__main__':
    main()
