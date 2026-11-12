#!/usr/bin/env python3
"""Focused slope bench of a scope read in a dirty state: a few changed pairs must not make the read dearer than the join it falls back to.

    PGHOST=.. PGPORT=.. python3 -I bench/steps/scope_dirty.py [--paths one|two] [--facts N] [--changed 1,20,200,2000] [--db NAME] [--keys 1,10]

A table of N facts (200,000 by default, on a tmpfs cluster) with its member key, a pairs relation of (member, tenant) with its journal index, and one scope index. For each number of changed
pairs (the pairs of the first dim_a_id values of the big tenant are deleted and inserted again: a journaled edit that leaves the rows as they were) a fresh fixture is built, and for a tiny and a big tenant, and for a
read of 1 and of 10 tenants, three arms are measured; --paths two builds the scope of two paths (a tenant is granted dim_a_id values and groups, a group has dim_a_id values) and edits the pairs of a tenant
no read asks for (other), of the big one and of the tiny one, directly granted (direct) or through a group that holds them (member), for 1 tenant reads: the scope units (`scope`, forced), the join as written (`join`, sift.agg_scope off) and the planner's choice (`chosen`). Each cell reports the
absolute instructions of the backend (perf stat, median of 3), the shared blocks hit / read of a cold run (shared buffers emptied) and of a warm one, and whether the rows are md5-equal to the plain
statement (sift.enable_agg off). The planner's pick is checked: `chosen` must be within TOL (3%) of the cheaper arm (60,000 instructions of a 2 million one); the exit code is 1 on a
differing md5 and 2 on a cell over the tolerance (reported, the run goes on). A change to the pairs of other keys than the read's (`other`, or the other tenant) costs the read nothing: `chosen` must be within TOL of the same cell of the first state, which has no change, and RECORD_INSTR instructions for each changed pair (its journal records are read and sorted).
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
# the journal records of a changed pair of other keys (two: the row ended and the row inserted) are read and sorted by the statement that reads the key: about 120 instructions each, 150 for a pair that was changed
RECORD_INSTR = 150
FLOOR = 0
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
        while len(runs) < n:
            perf = subprocess.Popen(['perf', 'stat', '-x,', '-e', 'instructions:u', '-p', str(self.pid)], stderr=subprocess.PIPE, text=True,
                                    preexec_fn=lambda: signal.signal(signal.SIGINT, signal.SIG_DFL))
            time.sleep(0.3)
            self.run(sql)
            perf.send_signal(signal.SIGINT)
            m = re.search(r'^(\d+),,instructions', perf.communicate()[1], re.M)
            if m:
                runs.append(int(m.group(1)))
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

# two paths: tenant 1 (big) is granted every 10th of 20,000 dim_a_id values and 50 groups of 40, tenant 2 (tiny) five values and a group of 20, tenants 3..60 thirty values and two groups of five
FIXTURE_TWO = """
SET client_min_messages = warning;
CREATE EXTENSION IF NOT EXISTS pg_sift; CREATE EXTENSION IF NOT EXISTS pg_buffercache;
DROP SCHEMA IF EXISTS sd CASCADE; CREATE SCHEMA sd; SET search_path = sd, public;
SET sift.scope_max_multiplicity = 8;
CREATE TABLE grants (id serial PRIMARY KEY, tenant_id int NOT NULL, dim_a_id int, group_id int) WITH (autovacuum_enabled = false);
CREATE TABLE memberships (id serial PRIMARY KEY, group_id int NOT NULL, dim_a_id int NOT NULL) WITH (autovacuum_enabled = false);
INSERT INTO grants (tenant_id, dim_a_id) SELECT 1, i FROM generate_series(10, 20000, 10) i;
INSERT INTO grants (tenant_id, group_id) SELECT 1, g FROM generate_series(1, 50) g;
INSERT INTO memberships (group_id, dim_a_id) SELECT g, (g - 1) * 40 + k FROM generate_series(1, 50) g, generate_series(1, 40) k;
INSERT INTO grants (tenant_id, dim_a_id) SELECT 2, i FROM generate_series(15001, 15005) i;
INSERT INTO grants (tenant_id, group_id) VALUES (2, 51);
INSERT INTO memberships (group_id, dim_a_id) SELECT 51, 16000 + k FROM generate_series(1, 20) k;
INSERT INTO grants (tenant_id, dim_a_id) SELECT t, 1 + (t * 7919 + k * 613) % 20000 FROM generate_series(3, 60) t, generate_series(1, 30) k ON CONFLICT DO NOTHING;
INSERT INTO grants (tenant_id, group_id) SELECT t, 100 + t * 2 + j FROM generate_series(3, 60) t, generate_series(0, 1) j;
INSERT INTO memberships (group_id, dim_a_id) SELECT 100 + t * 2 + j, 1 + (t * 104729 + j * 7001 + k * 389) % 20000 FROM generate_series(3, 60) t, generate_series(0, 1) j, generate_series(1, 5) k ON CONFLICT DO NOTHING;
CREATE UNIQUE INDEX grants_direct ON grants (dim_a_id, tenant_id);
CREATE UNIQUE INDEX grants_group ON grants (group_id, tenant_id);
CREATE UNIQUE INDEX memberships_by_group ON memberships (group_id, dim_a_id);
CREATE INDEX grants_by_dim_a ON grants USING sift_col (dim_a_id, tenant_id) WITH (track_changes = 'ctid');
CREATE INDEX grants_by_group ON grants USING sift_col (group_id, tenant_id) WITH (track_changes = 'ctid');
CREATE INDEX memberships_by_dim_a ON memberships USING sift_col (dim_a_id, group_id) WITH (track_changes = 'ctid');
CREATE TABLE facts (id int PRIMARY KEY, dim_a_id int, created_at date, kind text, amount int) WITH (autovacuum_enabled = false);
INSERT INTO facts SELECT i, 1 + (i::bigint * 7919) % 20000, date '2025-01-01' + i % 365, 'k' || i % 7, i % 100 FROM generate_series(1, @FACTS@) i;
CREATE INDEX facts_col ON facts USING sift_col (id, dim_a_id, created_at, kind, amount)
	WITH (scope = 'tenant_id: dim_a_id -> grants(dim_a_id, tenant_id) | grants(group_id, tenant_id) via memberships(group_id, dim_a_id)');
CREATE VIEW tenant_pairs AS
	SELECT g.dim_a_id AS member, g.tenant_id FROM grants g WHERE g.dim_a_id IS NOT NULL
	UNION
	SELECT m.dim_a_id, g.tenant_id FROM grants g JOIN memberships m ON m.group_id = g.group_id;
VACUUM (FREEZE, ANALYZE) facts, grants, memberships;
"""
# an edit deletes the first n rows of the grants (or of the memberships of the groups) of a tenant and inserts them again
def churn_grants(tenant, n):
    return (f"WITH d AS (DELETE FROM sd.grants WHERE id IN (SELECT id FROM sd.grants WHERE tenant_id = {tenant} AND dim_a_id IS NOT NULL ORDER BY id LIMIT {n}) RETURNING *) "
            "INSERT INTO sd.grants SELECT * FROM d")

def churn_members(tenant, n):
    return (f"WITH d AS (DELETE FROM sd.memberships WHERE id IN (SELECT m.id FROM sd.memberships m JOIN sd.grants g ON g.group_id = m.group_id WHERE g.tenant_id = {tenant} ORDER BY m.id LIMIT {n}) RETURNING *) "
            "INSERT INTO sd.memberships SELECT * FROM d")

EDITS_TWO = {'direct other': lambda n: churn_grants(40, n), 'member other': lambda n: churn_members(40, n), 'direct big': lambda n: churn_grants(1, n),
             'member big': lambda n: churn_members(1, n), 'direct tiny': lambda n: churn_grants(2, n), 'member tiny': lambda n: churn_members(2, n)}
SHAPES_TWO = {'1 tenant': 'SELECT e.kind, sum(e.amount), count(*) FROM sd.facts e WHERE e.dim_a_id IN (SELECT member FROM sd.tenant_pairs WHERE tenant_id = %d) GROUP BY 1'}


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


def states(paths, changed):
    """(label, fixture, shapes, tenants, edit) of every state: a fresh fixture, edited once"""
    if paths == 'one':
        return [(str(n), FIXTURE, SHAPES, TENANTS, EDIT % n if n else None) for n in changed]
    out = [('clean', FIXTURE_TWO, SHAPES_TWO, {'tiny': 2, 'big': 1}, None)]
    return out + [(f'{label} {n}', FIXTURE_TWO, SHAPES_TWO, {'tiny': 2, 'big': 1}, edit(n)) for label, edit in EDITS_TWO.items() for n in changed if n]


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--paths', default='one', choices=('one', 'two'))
    ap.add_argument('--facts', type=int, default=200000)
    ap.add_argument('--changed', default='0,1,20,200,2000')
    ap.add_argument('--keys', default='1,10')
    ap.add_argument('--db', default='scope_dirty')
    a = ap.parse_args()
    subprocess.run(['psql', '-X', '-q', '-U', 'postgres', '-d', 'postgres', '-c', f'DROP DATABASE IF EXISTS {a.db}', '-c', f'CREATE DATABASE {a.db}'], check=True, env=ENV)
    bad = over = 0
    clean = {}
    print('| state | tenant | read | arm | M instr | cold hit/read | warm hit/read | picked |\n|---|---|---|---|---|---|---|---|')
    for label, fixture, shapes, tenants, edit in states(a.paths, [int(x) for x in a.changed.split(',')]):
        s = Sess(a.db)
        s.run(fixture.replace('@FACTS@', str(a.facts)))
        if edit:
            s.run(edit)
        for tn, k in tenants.items():
            for shape, tmpl in shapes.items():
                if shape.split()[0] not in a.keys.split(','):
                    continue
                if shape.startswith('10') and tn != 'big':
                    continue
                sql = tmpl % ((k,) * tmpl.count('%d'))
                res, md5 = cell(s, sql)
                if len(set(md5.values())) != 1:
                    bad += 1
                    print(f'MD5 DIFFERS {label} {tn} {shape}: {md5}', file=sys.stderr)
                best = min(res['join'][0], res['scope'][0])
                pick = 'scope' if 'Scope Units' in res['chosen'][3] else 'join'
                if res['chosen'][0] > best * (1 + TOL) + FLOOR:
                    over += 1
                # a change to the pairs of other keys costs the read of this key nothing: the cell of the first state, which has no change, is its cost
                edited = None if not edit else 'big' if a.paths == 'one' else label.split()[1]
                clean.setdefault((tn, shape), res['chosen'][0])
                if edited not in (None, tn) and res['chosen'][0] > clean[(tn, shape)] * (1 + TOL) + RECORD_INSTR * int(label.split()[-1]):
                    over += 1
                    print(f'OTHER KEYS COST {label} {tn} {shape}: {res["chosen"][0] / 1e6:.2f} M, {clean[(tn, shape)] / 1e6:.2f} M clean', file=sys.stderr)
                for arm in ('join', 'scope', 'chosen'):
                    ins, cold, warm, _ = res[arm]
                    print(f"| {label} | {tn} | {shape} | {arm} | {ins / 1e6:.2f} | {cold[0]}/{cold[1]} | {warm[0]}/{warm[1]} | {pick if arm == 'chosen' else ''} |", flush=True)
        s.p.stdin.close()
        s.p.wait()
    print(f'md5 differences: {bad}; cells over {TOL:.0%} of the cheaper arm + {FLOOR / 1e6} M: {over}')
    sys.exit(1 if bad else 2 if over else 0)


if __name__ == '__main__':
    main()
