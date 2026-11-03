#!/usr/bin/env python3
"""Paired unit-generation records, including whole build/update/VACUUM costs and a heap oracle."""
import argparse
import json
import os
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path[:0] = [os.path.join(os.path.dirname(HERE), 'steps')]
import iolib
from duckdb import Session
from records import collect, schema, check


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--host', required=True)
    ap.add_argument('--port', required=True, type=int)
    ap.add_argument('--rows', required=True, type=int)
    ap.add_argument('--out', required=True)
    ap.add_argument('--reps', type=int, default=3)
    a = ap.parse_args()
    s = Session(a.port, 600, None, db='postgres', host=a.host)
    run = lambda sql: collect.run_all(s, sql)
    run('CREATE EXTENSION IF NOT EXISTS pg_sift; CREATE EXTENSION IF NOT EXISTS pg_buffercache')
    run('SET max_parallel_workers_per_gather = 0; SET max_parallel_maintenance_workers = 0')
    run('DROP TABLE IF EXISTS w, seed')
    run('CREATE TABLE seed(id int PRIMARY KEY, tenant_id int NOT NULL, counter_a int NOT NULL)')
    run('INSERT INTO seed SELECT i,i %% 60,i %% 10000 FROM generate_series(1,%d) i' % a.rows)
    run('VACUUM (FREEZE,ANALYZE) seed')
    tip = subprocess.check_output(['git', '-C', os.path.join(HERE, '../..'), 'rev-parse', '--short=12', 'HEAD'], text=True).strip()
    env = collect.Env(tip, 'unit generation updates', schema.run_id(tip), collect.host_cpus(collect.postmaster_pid(collect.backend_pid(s))),
                      reps=a.reps, cool=iolib.cool, maintenance='default')
    out = []
    index_sql = "CREATE INDEX w_c ON w USING sift_col(id,tenant_id,counter_a) WITH(track_updates='id',key_cut='id')"
    settle = "DO $$ BEGIN LOOP EXIT WHEN sift.col_maintenance_due('w_c',true) IS NULL; PERFORM sift.col_maintain('w_c'); PERFORM pg_sleep(0.01); END LOOP; PERFORM pg_sleep(0.05); LOOP PERFORM pg_stat_clear_snapshot(); EXIT WHEN NOT EXISTS (SELECT FROM pg_stat_activity WHERE backend_type='sift maintenance' OR backend_type='autovacuum worker'); PERFORM pg_sleep(0.01); END LOOP; END $$"
    pins = ['SET enable_seqscan=off', 'SET enable_indexscan=off', 'SET enable_indexonlyscan=off', 'SET enable_bitmapscan=off']
    reset_pins = ';'.join('RESET ' + q.split()[1].split('=')[0] for q in pins)
    oracle_sql = 'SELECT tenant_id,count(*),sum(counter_a),sum(id) FROM w GROUP BY tenant_id ORDER BY tenant_id'

    def save():
        schema.write(a.out, out)
        problems = check.check(schema.read(a.out))
        if problems:
            raise RuntimeError('\n'.join(problems))
        print('%s rows=%d records=%d' % (env.run_id, a.rows, len(out)), flush=True)

    def select_method(value):
        run("ALTER SYSTEM SET sift.eval_methods='%s'" % value)
        run('SELECT pg_reload_conf()')
        for _ in range(100):
            probe = subprocess.check_output(['psql', '-XAtq', '-h', a.host, '-p', str(a.port), '-U', 'postgres', '-d', 'postgres',
                                             '-c', "LOAD 'pg_sift'; SELECT current_setting('sift.eval_methods')"], text=True).strip()
            if probe == value:
                break
            time.sleep(.02)
        else:
            raise RuntimeError('evaluation setting was not reloaded for new workers')
        run("SET sift.eval_methods='%s'" % value)

    def fresh(index=True):
        run('DROP TABLE IF EXISTS w; CREATE TABLE w (LIKE seed INCLUDING ALL); INSERT INTO w SELECT * FROM seed ORDER BY id')
        if index:
            run(index_sql)
        run('VACUUM (FREEZE,ANALYZE) w')

    def verify(label):
        run(collect.FORCING['heap'] + ['SET enable_seqscan=on'])
        heap = s.run(oracle_sql)[1]
        run(['RESET sift.enable_agg', 'RESET sift.enable_col_scan'] + pins)
        plan = json.loads('\n'.join(s.run('EXPLAIN (FORMAT JSON) ' + oracle_sql)[1]))[0]['Plan']
        if collect.plan_structure(plan, *collect.catalog_of(s, 'w')) != 'sift':
            raise RuntimeError('oracle did not exercise sift')
        if s.run(oracle_sql)[1] != heap:
            raise RuntimeError('sift/heap mismatch: ' + label)
        run("SET sift.eval_methods=''")
        if s.run(oracle_sql)[1] != heap:
            raise RuntimeError('method-off/heap mismatch: ' + label)
        run("SET sift.eval_methods='%s'" % method_value)
        run(reset_pins)
        print('oracle ' + label + ' passed', flush=True)

    def subject(cell, state, kind='read'):
        dataset = '%s-%s-%s' % (*cell.rsplit('-',1)[:1], state, cell.rsplit('-',1)[1]) if kind == 'read' else cell
        return collect.Subject(dataset, a.rows, 'sorted', state, 'w', key='id', keys=a.rows, kind=kind,
                               structures='constraint,heap,sift', default_option=cell.rsplit('-',1)[1], churn=churn if state != 'fresh_vacuumed' else None,
                               settled=state == 'churned_vacuumed')

    def reads(cell, state):
        verify(cell + ':' + state)
        run(pins)
        for name, query in [('group-all', 'SELECT tenant_id,count(*),sum(counter_a) FROM w GROUP BY tenant_id'),
                            ('range-key', 'SELECT tenant_id,count(*),sum(counter_a) FROM w WHERE id<=%d GROUP BY tenant_id' % (a.rows // 100)),
                            ('filter-group', 'SELECT count(*),sum(counter_a) FROM w WHERE tenant_id=1')]:
            out.extend(collect.measure_statement(s, name, query, subject(cell, state), [collect.Arm('sift', method, settings=pins), collect.Arm('heap')] + ([collect.Arm('btree', settings=collect.FORCING['btree'] + ['SET enable_indexscan=on', 'SET enable_indexonlyscan=on', 'SET enable_bitmapscan=on'])] if name == 'range-key' else []), env, temps=('warm','cold')))
        save()
        run(reset_pins)

    churn = None
    for method in ('off','on'):
        method_value = 'unit_rewrite' if method == 'on' else ''
        select_method(method_value)
        fresh(False)
        out.extend(collect.measure_statement(s, 'build-index', index_sql, subject('unit-build-' + method, 'fresh_vacuumed', 'build'),
                   [collect.Arm('sift', method, settings=[], prepare=lambda: fresh(False), structures='constraint,heap,sift')], env, temps=('warm',)))
        save()
        def newly_built():
            fresh(False)
            run(index_sql)
        out.extend(collect.measure_statement(s, 'build-cleanup', ['VACUUM (FREEZE,ANALYZE) w', settle], subject('unit-build-' + method, 'fresh_vacuumed', 'maintenance'),
                   [collect.Arm('sift', method, settings=[], prepare=newly_built, structures='constraint,heap,sift')], env, temps=('warm',)))
        save()
    for share in (1,5,20):
        for pattern in ('clustered','random'):
            churn = 'update=%d%%;pattern=%s' % (share,pattern)
            pred = 'id<=%d' % (a.rows*share//100) if pattern == 'clustered' else '(id::bigint*7919)%%%d<%d' % (a.rows,a.rows*share//100)
            update_sql = 'UPDATE w SET counter_a=counter_a+1 WHERE ' + pred
            for method in ('off','on'):
                method_value = 'unit_rewrite' if method == 'on' else ''
                select_method(method_value)
                cell = 'unit-%d-%s-%s' % (share,pattern,method)
                fresh()
                reads(cell,'fresh_vacuumed')
                out.extend(collect.measure_statement(s, 'update-rows', update_sql, subject(cell,'fresh_vacuumed','load'),
                           [collect.Arm('sift',method,settings=[],prepare=fresh,structures='constraint,heap,sift')],env,temps=('warm',)))
                reads(cell,'churned_unvacuumed')
                def churned():
                    fresh()
                    run(update_sql)
                out.extend(collect.measure_statement(s, 'vacuum-and-settle', ['VACUUM (ANALYZE,INDEX_CLEANUP ON) w',settle], subject(cell,'churned_unvacuumed','maintenance'),
                           [collect.Arm('sift',method,settings=[],prepare=churned,structures='constraint,heap,sift')],env,temps=('warm',)))
                reads(cell,'churned_vacuumed')
                out.extend(collect.measure_statement(s, 'complete-cycle', [index_sql, 'VACUUM (FREEZE,ANALYZE) w', settle, update_sql,
                           'VACUUM (ANALYZE,INDEX_CLEANUP ON) w', settle], subject(cell,'fresh_vacuumed','maintenance'),
                           [collect.Arm('sift',method,settings=[],prepare=lambda: fresh(False),structures='constraint,heap,sift')],env,temps=('warm',)))
                save()
    run('DROP TABLE w,seed')
    s.close()


if __name__ == '__main__':
    main()
