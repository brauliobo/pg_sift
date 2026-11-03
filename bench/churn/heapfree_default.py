#!/usr/bin/env python3
"""What the default track_updates = 'auto' costs a write: the same statements on a table with the index as it was (track_updates = 'off'), with the default (the primary key, tracked), with
the default and track_deletes = on, and on 1, 2 and 3 sift_col indexes of one table, on a table with a primary key, without one, partitioned, unlogged and temporary.

    python3 -I bench/churn/heapfree_default.py --host DIR --port N --out FILE [--rows 200000] [--reps 3] [--part pk,nokey,...] [--variants off1,def1,...]

Records as heapfree_records.py --write (instructions, WAL bytes, blocks, ms of one statement on a fresh, vacuumed table). The variants: heap_only (no sift index), offN (N indexes with
track_updates = 'off'), defN (N indexes with the default), def1_deletes (the default and track_deletes = on).
"""
import argparse
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import heapfree_records as H  # noqa: E402

collect, schema, iolib, Session = H.collect, H.schema, H.iolib, H.Session

SHAPES = {
    'pk': ('CREATE TABLE w (id int PRIMARY KEY, tenant_id int NOT NULL, counter_a int, label text) WITH (autovacuum_enabled = false, fillfactor = 100)', 'constraint,heap'),
    'nokey': ('CREATE TABLE w (id int, tenant_id int NOT NULL, counter_a int, label text) WITH (autovacuum_enabled = false, fillfactor = 100)', 'heap'),
    'part': ('CREATE TABLE w (id int PRIMARY KEY, tenant_id int NOT NULL, counter_a int, label text) PARTITION BY HASH (id); '
             'CREATE TABLE w_0 PARTITION OF w FOR VALUES WITH (modulus 4, remainder 0) WITH (autovacuum_enabled = false, fillfactor = 100); '
             'CREATE TABLE w_1 PARTITION OF w FOR VALUES WITH (modulus 4, remainder 1) WITH (autovacuum_enabled = false, fillfactor = 100); '
             'CREATE TABLE w_2 PARTITION OF w FOR VALUES WITH (modulus 4, remainder 2) WITH (autovacuum_enabled = false, fillfactor = 100); '
             'CREATE TABLE w_3 PARTITION OF w FOR VALUES WITH (modulus 4, remainder 3) WITH (autovacuum_enabled = false, fillfactor = 100)', 'constraint,heap'),
    'unlogged': ('CREATE UNLOGGED TABLE w (id int PRIMARY KEY, tenant_id int NOT NULL, counter_a int, label text) WITH (autovacuum_enabled = false, fillfactor = 100)', 'constraint,heap'),
    'temp': ('CREATE TEMP TABLE w (id int PRIMARY KEY, tenant_id int NOT NULL, counter_a int, label text) WITH (autovacuum_enabled = false, fillfactor = 100)', 'constraint,heap'),
}
COLS = ('id, tenant_id, counter_a', 'id, counter_a, label', 'id, tenant_id')
OPTS = {'off': " WITH (track_updates = 'off')", 'def': '', 'deletes': ' WITH (track_deletes = on)'}
# (name, number of indexes, option set); heap_only has no index
VARIANTS = (('heap_only', 0, 'off'), ('off1', 1, 'off'), ('def1', 1, 'def'), ('def1_deletes', 1, 'deletes'), ('off2', 2, 'off'), ('def2', 2, 'def'), ('off3', 3, 'off'), ('def3', 3, 'def'))
STATEMENTS = (('insert_5pct', 'loaded', 'INSERT INTO w SELECT i, i % 10, i % 1000, md5(i::text) FROM generate_series({n} + 1, {n} + {n} / 20) i'),
              ('update_1_row', 'loaded', 'UPDATE w SET counter_a = counter_a + 1 WHERE id = {n} / 2'),
              ('update_100_rows', 'loaded', 'UPDATE w SET counter_a = counter_a + 1 WHERE id BETWEEN {n} / 2 AND {n} / 2 + 99'),
              ('update_5pct_covered', 'loaded', 'UPDATE w SET counter_a = counter_a + 1 WHERE id % 20 = 0'),
              ('update_5pct_uncovered', 'loaded', 'UPDATE w SET label = label WHERE id % 20 = 0'),
              ('update_key_1pct', 'loaded', 'UPDATE w SET id = id + {n} * 10 WHERE id % 100 = 1'),
              ('delete_2pct', 'loaded', 'DELETE FROM w WHERE id % 50 = 0'),
              ('bulk_insert', 'empty', 'INSERT INTO w SELECT i, i % 10, i % 1000, md5(i::text) FROM generate_series(1, {n}) i'),
              ('copy_load', 'empty', "COPY w FROM '{csv}' (FORMAT csv)"),
              ('create_index', 'unindexed', 'CREATE INDEX w_c ON w USING sift_col ({cols}){opt}'),
              ('reindex', 'loaded', 'REINDEX INDEX w_c'),
              ('enable_deletes', 'updated', 'ALTER INDEX w_c0 SET (track_deletes = on)'))
# the statements the other shapes run: the append, the update and the delete
SHORT = ('insert_5pct', 'update_5pct_covered', 'delete_2pct')


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--host', required=True)
    ap.add_argument('--port', type=int, required=True)
    ap.add_argument('--out', required=True)
    ap.add_argument('--rows', type=int, default=200000)
    ap.add_argument('--reps', type=int, default=3)
    ap.add_argument('--db', default='heapfree_default')
    ap.add_argument('--tip', default=None)
    ap.add_argument('--note', default=None)
    ap.add_argument('--shapes', default=','.join(SHAPES))
    ap.add_argument('--variants', default=None)
    ap.add_argument('--statements', default=None)
    a = ap.parse_args()
    admin = Session(a.port, 600, None, db='postgres', host=a.host)
    admin.run('DROP DATABASE IF EXISTS %s' % a.db)
    admin.run('CREATE DATABASE %s' % a.db)
    s = Session(a.port, 600, None, db=a.db, host=a.host)
    s.run('CREATE EXTENSION pg_sift')
    s.run('SET client_min_messages = warning')
    t = H.tip(a)
    env = collect.Env(t, H.harness(a, 'heapfree default'), schema.run_id(t), collect.host_cpus(collect.postmaster_pid(collect.backend_pid(s))), reps=a.reps, cool=iolib.cool, maintenance='disabled')
    n = a.rows
    csv = '/dev/shm/heapfree_default_%d.csv' % n
    with open(csv, 'w') as f:
        import hashlib
        for i in range(1, n + 1):
            f.write('%d,%d,%d,%s\n' % (i, i % 10, i % 1000, hashlib.md5(str(i).encode()).hexdigest()))
    out = []

    def run(sql):
        _, _, err, *_ = s.run(sql)
        if err and 'ERROR' in err:
            raise RuntimeError('%s: %s' % (sql, err))

    def fresh(shape, nidx, opt, mode):
        def prepare():
            run('DROP TABLE IF EXISTS w')
            run(SHAPES[shape][0])
            if mode != 'empty':
                run("INSERT INTO w SELECT i, i %% 10, i %% 1000, md5(i::text) FROM generate_series(1, %d) i" % n)
            if mode != 'unindexed':
                for k in range(nidx):
                    run('CREATE INDEX w_c%d ON w USING sift_col (%s)%s' % (k, COLS[k], OPTS[opt]))
            if mode == 'updated':
                run('UPDATE w SET counter_a = counter_a + 1 WHERE id % 20 = 0')   # 5% of the rows have a dead version: no VACUUM
            elif mode != 'empty':
                run('VACUUM (FREEZE, ANALYZE) w')
            s.run('CHECKPOINT')
        return prepare

    for shape in a.shapes.split(','):
        for stmt, mode, text in STATEMENTS:
            if a.statements and stmt not in a.statements.split(','):
                continue
            if shape != 'pk' and stmt not in SHORT:
                continue
            for vname, nidx, opt in VARIANTS:
                if a.variants and vname not in a.variants.split(','):
                    continue
                if shape != 'pk' and (nidx > 1 or opt == 'deletes'):
                    continue
                if stmt in ('create_index', 'reindex', 'enable_deletes') and (nidx == 0 or nidx > 1 or (stmt == 'enable_deletes' and opt == 'deletes')):
                    continue
                sql = text.format(n=n, csv=csv, cols=COLS[0], opt=OPTS[opt])
                if stmt == 'reindex':
                    sql = 'REINDEX INDEX w_c0'
                if stmt == 'enable_deletes' and opt == 'off':   # without the key tracked the way to track_deletes is the option and a rebuild
                    sql = ['ALTER INDEX w_c0 RESET (track_updates)', 'REINDEX INDEX w_c0', 'ALTER INDEX w_c0 SET (track_deletes = on)']
                sub = collect.Subject('heapfree_default', n, 'arrival', 'fresh_vacuumed', 'w', kind='load', key='tenant_id', keys=10, structures=SHAPES[shape][1] + (',sift' if nidx else ''), variant='%s/%s' % (shape, vname))
                arm = collect.Arm('sift' if nidx else 'heap', vname, settings=[], prepare=fresh(shape, nidx, opt, mode), structures=sub.structures)
                out += collect.measure_statement(s, stmt, sql, sub, [arm], env, temps=('warm',), hash_text=text)
                print(shape, stmt, vname, flush=True)
    schema.write(a.out, out)
    from records import check
    for p in check.check(schema.read(a.out)):
        print(p)
    print('run_id %s: %d records' % (env.run_id, len(out)))


if __name__ == '__main__':
    main()
