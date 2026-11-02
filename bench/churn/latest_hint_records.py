#!/usr/bin/env python3
"""Records for transaction-local supersede hints; each switch value is an explicit dataset.

The datasets differ only in the named method. The settings hash keeps that difference;
compare pairs only after checking all other settings and the SQL hash. Each write repeat
starts with the same freshly indexed rows. The short-transaction arm commits every update.
"""
import argparse
import json
import os
import subprocess
import sys

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
    ap.add_argument('--out')
    ap.add_argument('--updates', type=int, default=10000)
    ap.add_argument('--keys', default='100,10000')
    ap.add_argument('--patterns', default='clustered,random')
    ap.add_argument('--transactions', default='one,short')
    ap.add_argument('--reps', type=int, default=3)
    ap.add_argument('--build-only', action='store_true')
    ap.add_argument('--vacuum-only', action='store_true')
    ap.add_argument('--oracle-only', action='store_true')
    a = ap.parse_args()
    if not a.oracle_only and not a.out:
        ap.error('--out is required for measurement')
    s = Session(a.port, 600, None, db='postgres', host=a.host)

    def run(sql):
        return collect.run_all(s, sql)

    run('CREATE EXTENSION IF NOT EXISTS pg_sift')
    run('CREATE EXTENSION IF NOT EXISTS pg_buffercache')
    run('SET max_parallel_workers_per_gather = 0')
    run('SET max_parallel_maintenance_workers = 0')
    run('DROP TABLE IF EXISTS w, seed')
    run('CREATE TABLE seed (id int PRIMARY KEY, tenant_id int NOT NULL, counter_a int NOT NULL)')
    run('INSERT INTO seed SELECT i, i %% 60, i %% 10000 FROM generate_series(1, %d) i' % a.rows)
    run('VACUUM (FREEZE, ANALYZE) seed')
    tip = subprocess.check_output(['git', '-C', os.path.join(HERE, '../..'), 'rev-parse', '--short=12', 'HEAD'], text=True).strip()
    env = collect.Env(tip, 'latest hint transactions', schema.run_id(tip), collect.host_cpus(collect.postmaster_pid(collect.backend_pid(s))),
                      reps=a.reps, cool=iolib.cool, maintenance='default')
    out = []

    def save():
        schema.write(a.out, out)
        problems = check.check(schema.read(a.out))
        if problems:
            raise RuntimeError('\n'.join(problems))
        print('run_id %s rows %d records %d' % (env.run_id, a.rows, len(out)), flush=True)

    def fresh(index=True):
        run('DROP TABLE IF EXISTS w')
        run('CREATE TABLE w (LIKE seed INCLUDING ALL)')
        run('INSERT INTO w SELECT * FROM seed ORDER BY id')
        if index:
            run("CREATE INDEX w_c ON w USING sift_col (id, tenant_id, counter_a) WITH (track_updates = 'id')")
        run('VACUUM (FREEZE, ANALYZE) w')

    def verify(updates):
        query = 'SELECT tenant_id, count(*), sum(counter_a), sum(id) FROM w GROUP BY tenant_id ORDER BY tenant_id'
        run('SET enable_indexscan = off; SET enable_indexonlyscan = off; SET enable_bitmapscan = off; SET sift.enable_agg = off')
        heap = s.run(query)[1]
        total = int(collect.one(s, 'SELECT sum(counter_a) FROM w')[0])
        if total != sum(i % 10000 for i in range(1, a.rows + 1)) + updates:
            raise RuntimeError('heap update-count oracle failed')
        run('RESET enable_indexscan; RESET enable_indexonlyscan; RESET enable_bitmapscan; RESET sift.enable_agg; SET enable_seqscan = off')
        plan = json.loads('\n'.join(s.run('EXPLAIN (FORMAT JSON) ' + query)[1]))[0]['Plan']
        if collect.plan_structure(plan, *collect.catalog_of(s, 'w')) != 'sift':
            raise RuntimeError('oracle did not exercise sift')
        if s.run(query)[1] != heap:
            raise RuntimeError('sift aggregate differs from heap')
        run('RESET enable_seqscan')

    def read_state(label, state, method, churn=None):
        for name, sql in [('group-all', 'SELECT tenant_id, count(*), sum(counter_a) FROM w GROUP BY tenant_id'),
                          ('point-key', 'SELECT counter_a FROM w WHERE id = 1')]:
            subj = collect.Subject('latest-hint-read-%s-%s' % (label, method), a.rows, 'sorted', state, 'w', key='id', keys=a.rows,
                                   structures='constraint,heap,sift', churn=churn, settled=label == 'vacuumed')
            out.extend(collect.measure_statement(s, name, sql, subj, [collect.Arm('sift', method), collect.Arm('heap')], env, temps=('warm', 'cold')))
        save()

    fresh()
    if a.build_only:
        for method in ('off', 'on'):
            run("SET sift.eval_methods = '%s'" % ('latest_hint' if method == 'on' else ''))
            subject = collect.Subject('latest-hint-build-' + method, a.rows, 'sorted', 'fresh_vacuumed', 'w', key='id', keys=a.rows,
                                      kind='maintenance', structures='constraint,heap,sift')
            out.extend(collect.measure_statement(s, 'build-index', "CREATE INDEX w_c ON w USING sift_col (id, tenant_id, counter_a) WITH (track_updates = 'id')", subject,
                       [collect.Arm('sift', method, settings=[], prepare=lambda: fresh(False), structures=subject.structures)], env, temps=('warm',)))
            save()
        run('DROP TABLE w, seed')
        s.close()
        return
    for keys in map(int, a.keys.split(',')):
        for pattern in a.patterns.split(','):
            ids = [j + 1 if pattern == 'clustered' else (j * 7919) % a.rows + 1 for j in range(keys)]
            updates = ['UPDATE w SET counter_a = counter_a + 1 WHERE id = %d' % ids[j % keys] for j in range(a.updates)]
            for tx in a.transactions.split(','):
                statements = ['BEGIN'] + updates + ['COMMIT'] if tx == 'one' else updates
                for method in ('off', 'on'):
                    run("SET sift.eval_methods = '%s'" % ('latest_hint' if method == 'on' else ''))
                    cell = 'latest-hint-%s-%d-keys-%s-tx-%s' % (pattern, keys, tx, method)
                    if a.oracle_only:
                        fresh()
                        verify(0)
                        run(statements)
                        verify(a.updates)
                        run('VACUUM (ANALYZE) w')
                        verify(a.updates)
                        print('oracle %s rows %d passed fresh/churned/vacuumed' % (cell, a.rows), flush=True)
                        continue
                    if a.vacuum_only:
                        def churned():
                            fresh()
                            run(statements)
                        subject = collect.Subject(cell + '-vacuum', a.rows, 'sorted', 'churned_unvacuumed', 'w', key='id', keys=a.rows,
                                                  kind='maintenance', structures='constraint,heap,sift', protocol='updates=%d;transactions=%s' % (a.updates, tx))
                        out.extend(collect.measure_statement(s, 'vacuum-after-updates', ['VACUUM (ANALYZE) w',
                                    "DO $$ BEGIN LOOP EXIT WHEN sift.col_maintenance_due('w_c', true) IS NULL; PERFORM sift.col_maintain('w_c'); PERFORM pg_sleep(0.01); END LOOP; END $$"], subject,
                                   [collect.Arm('sift', method, settings=[], prepare=churned, structures=subject.structures)], env, temps=('warm',)))
                        save()
                        continue
                    subject = collect.Subject(cell, a.rows, 'sorted', 'fresh_vacuumed', 'w', key='id', keys=a.rows,
                                              kind='load', structures='constraint,heap,sift', protocol='updates=%d;transactions=%s' % (a.updates, tx))
                    out.extend(collect.measure_statement(s, 'hot-key-updates', statements, subject,
                               [collect.Arm('sift', method, settings=[], prepare=fresh, structures=subject.structures)], env, temps=('warm',)))
                    verify(a.updates)
                    save()
                    # Read effects are sampled on one repeated hot-key cell, in each requested state.
                    if keys == 100 and pattern == 'clustered' and tx == 'one':
                        read_state('churned', 'churned_unvacuumed', method, 'updates=%d;keys=%d;transactions=one' % (a.updates, keys))
                        run('VACUUM (ANALYZE) w')
                        read_state('vacuumed', 'churned_vacuumed', method, 'updates=%d;keys=%d;transactions=one' % (a.updates, keys))
                        fresh()
                        read_state('fresh', 'fresh_vacuumed', method)
    run('DROP TABLE w, seed')
    s.close()


if __name__ == '__main__':
    main()
