#!/usr/bin/env python3
"""Focused slope bench of a scope read in a dirty state: a few changed pairs must not make the read dearer than the join it falls back to.

    PGHOST=.. PGPORT=.. python3 -I bench/steps/scope_dirty.py [--facts N] [--changed 1,20,200,2000] [--db NAME] [--keys 1,10]

A table of N facts (200,000 by default, on a tmpfs cluster) with its member key, a pairs relation of (member, tenant) with its journal index, and one scope index. For each number of changed
pairs (the pairs of the first dim_a_id values of the big tenant are deleted and inserted again: a journaled edit that leaves the rows as they were) a fresh fixture is built, and for a tiny and a big tenant, and for a
read of 1 and of 10 tenants, three arms are measured: the scope units (`scope`, forced), the join as written (`join`, sift.agg_scope off) and the planner's choice (`chosen`). Each cell reports the
absolute instructions of the backend (perf stat, median of 3), the shared blocks hit / read of a cold run (shared buffers emptied) and of a warm one, and whether the rows are md5-equal to the plain
statement (sift.enable_agg off). The planner's pick is checked: `chosen` must be within TOL (3%) of the cheaper arm plus the planning floor FLOOR (0.3 M instructions); the exit code is 1 on a
differing md5 and 2 on a cell over the tolerance (reported, the run goes on).
"""
import argparse
import os
import re
import signal
import statistics
import subprocess
import sys
import time

TOL = 0.03
FLOOR = 0.3e6
ARMS = (('join', 'SET sift.enable_agg = on; SET sift.agg_scope = off; SET sift.test_scope_patch = off'),
        ('scope', 'SET sift.enable_agg = on; SET sift.agg_scope = on; SET sift.test_scope_patch = on'),
        ('chosen', 'SET sift.enable_agg = on; SET sift.agg_scope = on; SET sift.test_scope_patch = off'))
PLAIN = 'SET sift.enable_agg = off'
ENV = dict(os.environ, LC_ALL='C')


class Sess:
    def __init__(self, db):
        self.p = subprocess.Popen(['psql', '-X', '-q', '-At', '-U', 'postgres', '-d', db], stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, env=ENV)
        self.run('SET max_parallel_workers_per_gather = 0; SET client_min_messages = warning; SET jit = off; SET sift.scope_max_multiplicity = 8')
        self.pid = int(next(l for l in self.run('SELECT pg_backend_pid()') if l.isdigit()))

    def run(self, sql):
        self.p.stdin.write(sql.rstrip().rstrip(';') + ";\nSELECT '__done__';\n")
        self.p.stdin.flush()
        out = []
        while True:
            line = self.p.stdout.readline()
            if line.strip() == '__done__':
                break
            if line == '':
                raise RuntimeError('psql ended: ' + '\n'.join(out))
            out.append(line.rstrip('\n'))
        if any(l.startswith('ERROR') for l in out):
            raise RuntimeError('\n'.join(out))
        return out

    def instr(self, sql, n=3):
        self.run(sql)
        runs = []
        for _ in range(n):
            perf = subprocess.Popen(['perf', 'stat', '-x,', '-e', 'instructions:u', '-p', str(self.pid)], stderr=subprocess.PIPE, text=True)
            time.sleep(0.3)
            self.run(sql)
            perf.send_signal(signal.SIGINT)
            runs.append(int(re.search(r'^(\d+),,instructions', perf.communicate()[1], re.M).group(1)))
        return statistics.median(runs)

    def blocks(self, sql):
        plan = '\n'.join(self.run('EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY OFF) ' + sql))
        m = re.search(r'Buffers: shared (?:hit=(\d+))?(?: )?(?:read=(\d+))?', plan)
        return int(m.group(1) or 0) if m else 0, int(m.group(2) or 0) if m else 0, plan

    def md5(self, sql):
        return self.run(f"SELECT md5(string_agg(t::text, '|' ORDER BY t::text)) FROM ({sql}) t")[0]


FIXTURE = """
SET client_min_messages = warning;
CREATE EXTENSION IF NOT EXISTS pg_sift; CREATE EXTENSION IF NOT EXISTS pg_buffercache;
DROP SCHEMA IF EXISTS sd CASCADE; CREATE SCHEMA sd; SET search_path = sd, public;
SET sift.scope_max_multiplicity = 8;
-- 2,000 dim_a_id values; tenant 1 is big (every one), tenant 2 tiny (20 of them), tenants 3..12 hold a tenth each
CREATE TABLE tenant_scope (dim_a_id int NOT NULL, tenant_id int NOT NULL, PRIMARY KEY (dim_a_id, tenant_id)) WITH (autovacuum_enabled = false);
INSERT INTO tenant_scope SELECT m, 1 FROM generate_series(1, 2000) m;
INSERT INTO tenant_scope SELECT m, 2 FROM generate_series(1, 20) m;
INSERT INTO tenant_scope SELECT m, 3 + m % 10 FROM generate_series(1, 2000) m;
CREATE INDEX tenant_scope_journal ON tenant_scope USING sift_col (dim_a_id, tenant_id) WITH (track_changes = 'ctid');
CREATE TABLE events (id int PRIMARY KEY, dim_a_id int, created_at date, kind text, amount int) WITH (autovacuum_enabled = false);
INSERT INTO events SELECT i, 1 + (i * 7919) % 2000, date '2025-01-01' + i % 365, 'k' || i % 7, i % 100 FROM generate_series(1, @FACTS@) i;
CREATE INDEX events_col ON events USING sift_col (id, dim_a_id, created_at, kind, amount) WITH (scope = 'tenant_id: dim_a_id -> tenant_scope(dim_a_id, tenant_id)');
VACUUM (FREEZE, ANALYZE) events, tenant_scope;
"""
EDIT = """WITH d AS (DELETE FROM sd.tenant_scope WHERE tenant_id = 1 AND dim_a_id IN (SELECT dim_a_id FROM sd.tenant_scope WHERE tenant_id = 1 ORDER BY dim_a_id LIMIT %d) RETURNING *)
INSERT INTO sd.tenant_scope SELECT * FROM d"""
SHAPES = {'1 tenant': 'SELECT e.kind, sum(e.amount), count(*) FROM sd.events e WHERE e.dim_a_id IN (SELECT dim_a_id FROM sd.tenant_scope WHERE tenant_id = %d) GROUP BY 1',
          '10 tenants': 'SELECT e.kind, sum(e.amount), count(*) FROM sd.events e WHERE e.dim_a_id IN (SELECT dim_a_id FROM sd.tenant_scope WHERE tenant_id BETWEEN %d AND %d + 9) GROUP BY 1'}
TENANTS = {'tiny': 2, 'big': 1, 'mid': 3}


def cell(s, sql):
    """{arm: (instructions, cold (hit, read), warm (hit, read), plan)} and the md5 of every arm"""
    out, md5 = {}, {}
    s.run(PLAIN)
    md5['plain'] = s.md5(sql)
    for arm, settings in ARMS:
        s.run(settings)
        md5[arm] = s.md5(sql)
        s.run('SELECT pg_buffercache_evict_all()')
        h, r, plan = s.blocks(sql)
        ins = s.instr(sql)
        wh, wr, plan = s.blocks(sql)
        out[arm] = (ins, (h, r), (wh, wr), plan)
    return out, md5


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--facts', type=int, default=200000)
    ap.add_argument('--changed', default='0,1,20,200,2000')
    ap.add_argument('--keys', default='1,10')
    ap.add_argument('--db', default='scope_dirty')
    a = ap.parse_args()
    subprocess.run(['psql', '-X', '-q', '-U', 'postgres', '-d', 'postgres', '-c', f'DROP DATABASE IF EXISTS {a.db}', '-c', f'CREATE DATABASE {a.db}'], check=True, env=ENV)
    bad = over = 0
    print('| changed pairs | tenant | read | arm | M instr | cold hit/read | warm hit/read | picked |\n|---|---|---|---|---|---|---|---|')
    for n in [int(x) for x in a.changed.split(',')]:
        s = Sess(a.db)
        s.run(FIXTURE.replace('@FACTS@', str(a.facts)))
        if n:
            s.run(EDIT % n)
        for tn, k in TENANTS.items():
            for shape, tmpl in SHAPES.items():
                if shape.split()[0] not in a.keys.split(','):
                    continue
                if shape.startswith('10') and tn != 'big':
                    continue
                sql = tmpl % ((k,) * tmpl.count('%d'))
                res, md5 = cell(s, sql)
                if len(set(md5.values())) != 1:
                    bad += 1
                    print(f'MD5 DIFFERS n={n} {tn} {shape}: {md5}', file=sys.stderr)
                best = min(res['join'][0], res['scope'][0])
                pick = 'scope' if 'Scope Units' in res['chosen'][3] else 'join'
                if res['chosen'][0] > best * (1 + TOL) + FLOOR:
                    over += 1
                for arm in ('join', 'scope', 'chosen'):
                    ins, cold, warm, _ = res[arm]
                    print(f"| {n} | {tn} | {shape} | {arm} | {ins / 1e6:.2f} | {cold[0]}/{cold[1]} | {warm[0]}/{warm[1]} | {pick if arm == 'chosen' else ''} |", flush=True)
        s.p.stdin.close()
        s.p.wait()
    print(f'md5 differences: {bad}; cells over {TOL:.0%} of the cheaper arm + {FLOOR / 1e6} M: {over}')
    sys.exit(1 if bad else 2 if over else 0)


if __name__ == '__main__':
    main()
