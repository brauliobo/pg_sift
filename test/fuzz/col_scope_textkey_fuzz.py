#!/usr/bin/env python3
"""Differential fuzz of a scope whose key or member is text, varchar or uuid (test/t/col_scope_textkey_fuzz.sh runs it).

  test/fuzz/col_scope_textkey_fuzz.py [--seeds 1-6] [--rows 8000] [--steps 8] [--hash-bits 0] [--two-paths]      needs PGHOST / PGPORT of a cluster with pg_sift and pg_sift_test, SIFT_FUZZ_OUT

A seed (seed % 4: the key is text, varchar, uuid or an integer; seed // 4 % 4: the member is an integer, text, varchar facts with text pairs, or uuid; the string member columns are generated columns of an
integer, so every churn step below moves them as it moves the integer; one of them is the empty string and some facts have none) builds events, a table of (member, key) pairs whose keys are strings that differ in their bytes (a case, an accent, a space, a long one), its journal index and a
sift_col index with a scope on the pairs, and draws steps: batches of fact INSERTs, DELETEs and UPDATEs (the value, the member), pair INSERTs, DELETEs, key moves, UPDATEs of the key of a pair, pairs with
keys that are new (the value store has never seen them: their pairs are all delta until the next build), a key that leaves every pair, aborted transactions and savepoints, reads inside a transaction,
promotion, merge, VACUUM and REINDEX, with a REPEATABLE READ reader opened before the churn. After every step a battery (IN of one key and of a list with a key nobody has, EXISTS, a join for each
pair, a group by the key of every key and of a list, a table of keys, a time window, a top-N) must give the rows of sift.enable_agg = off, and sift.col_verify must pass; the reader must read, through the
scope and through the plain plan, the rows of its first read. A read is either stored from the units or says a documented Scope Fallback reason; right after a build the reads of one key are stored.
With --two-paths the pairs are the union of the direct pairs (tenant_scope) and the pairs a key gets through the groups it is granted (grp_scope, memberships: the view pair_set), the index has the scope of two paths, and
the churn also adds, deletes and moves grants of groups and memberships; the three relations have the key or member of the seed's types, the group is an integer.
"""
import argparse, collections, hashlib, os, random, re, sys, time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from col_twin_fuzz import Sess, Fail, ERR, SETUP, OUT

TWO = False
FUZZ_SETUP = "LOAD 'pg_sift'; " + SETUP + " SET sift.agg_force = on; SET jit = off; SET sift.scope_max_multiplicity = 8;"

LIB = r"""
CREATE OR REPLACE FUNCTION tk_cmp(q text) RETURNS text LANGUAGE plpgsql AS $$
DECLARE a text; b text; l text; plan text := ''; on_ text := current_setting('sift.enable_agg');
BEGIN
	EXECUTE 'SELECT coalesce(md5(string_agg(t::text, ''|'' ORDER BY t::text)), '''') FROM (' || q || ') t' INTO a;
	FOR l IN EXECUTE 'EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS OFF) ' || q LOOP plan := plan || l || E'\n'; END LOOP;
	PERFORM set_config('sift.enable_agg', 'off', false);
	EXECUTE 'SELECT coalesce(md5(string_agg(t::text, ''|'' ORDER BY t::text)), '''') FROM (' || q || ') t' INTO b;
	PERFORM set_config('sift.enable_agg', on_, false);
	RETURN (a = b)::text || '|' || (plan ~ 'Scope Index')::text || '|' || coalesce(substring(plan from 'Scope Fallback: ([^\n]*)'), '') || '|' || b;
END $$;
"""

DOCUMENTED = re.compile(r'^(unfilled|snapshot|too many changed keys|top-n, changed pairs|patch dearer than the entity path|dedupe .*|column .* not in the scope units|.*journal.*|.*multiset.*|.*stamp.*|.*changed.*)$')


def lit(ktype, n):
    """The key number n as a SQL literal of the seed's type: strings that differ in their bytes."""
    if ktype == 'int':
        return str(n)
    if ktype == 'uuid':
        h = hashlib.md5(f'k{n}'.encode()).hexdigest()
        return f"'{h[:8]}-{h[8:12]}-{h[12:16]}-{h[16:20]}-{h[20:]}'::uuid"
    s = ('tenant-%d' % n, 'Tenant %d ' % n, 'ténant-%d' % n, 'x' * (1 + n % 40) + str(n))[n % 4]
    return "'" + s.replace("'", "''") + "'::text"


class Seed:
    def __init__(self, seed, rows):
        r = self.rng = random.Random(seed * 7919 + 5)
        self.ktype = ['text', 'varchar', 'uuid', 'int'][seed % 4]
        self.mode = ['int', 'text', 'varchar', 'uuid'][seed // 4 % 4]
        self.mc = 'dim_a_id' if self.mode == 'int' else 'account'     # the member column of the pairs and of the index
        self.nkeys = r.choice([6, 12, 30])
        self.dim = r.choice([40, 150, 400])
        self.rows = rows
        self.next_id = rows + 1
        self.off = 0
        self.keys = list(range(1, self.nkeys + 1))
        self.spare = list(range(self.nkeys + 1, self.nkeys + 41))
        self.segkb = r.choice([0, 64, 256])
        self.two = TWO
        self.groups = r.choice([8, 25])
        self.pairs = 'pair_set' if self.two else 'tenant_scope'

    def k(self, n):
        return lit(self.ktype, n)

    def klist(self, ns):
        return ', '.join(self.k(n) for n in ns)

    def member_sql(self):
        """The function that makes the member of a number, its type on the facts and on the pairs."""
        if self.mode == 'int':
            return '', 'int', 'int', ''
        body = ("CASE WHEN n IS NULL THEN NULL WHEN n = 7 THEN '' WHEN n % 4 = 0 THEN 'acct-' || n WHEN n % 4 = 1 THEN 'Acct ' || n || ' ' WHEN n % 4 = 2 THEN 'ácct-' || n "
                "ELSE repeat('x', 1 + n % 40) || n END")
        if self.mode == 'uuid':
            body = "md5(n::text)::uuid"
        rt = 'uuid' if self.mode == 'uuid' else 'text'
        ft = 'varchar(300)' if self.mode == 'varchar' else rt
        return f"CREATE OR REPLACE FUNCTION acc(n int) RETURNS {rt} LANGUAGE sql IMMUTABLE AS $$ SELECT {body} $$;\n", ft, rt, 'acc'

    def setup_sql(self):
        t = self.ktype
        fn, ft, rt, acc = self.member_sql()
        mc = self.mc
        gen = (lambda ty: f", account {ty} GENERATED ALWAYS AS ({acc}(dim_a_id)) STORED") if acc else (lambda ty: '')
        vals = ', '.join(f'({n}, {self.k(n)})' for n in self.keys + self.spare)
        return f"""{f"SET sift.test_col_segment_kb = {self.segkb};" if self.segkb else ""}
DROP TABLE IF EXISTS events, tenant_scope, keys_n, grp_scope, memberships CASCADE;
DROP FUNCTION IF EXISTS acc(int);
{fn}
CREATE TABLE keys_n (n int PRIMARY KEY, k {t} NOT NULL UNIQUE);
INSERT INTO keys_n VALUES {vals};
CREATE TABLE tenant_scope (dim_a_id int NOT NULL, tenant_id {t} NOT NULL{gen(rt).replace('STORED', 'STORED NOT NULL')}, PRIMARY KEY (dim_a_id, tenant_id){', UNIQUE (account, tenant_id)' if acc else ''}) WITH (autovacuum_enabled = false);
INSERT INTO tenant_scope SELECT m, k.k FROM generate_series(1, {self.dim}) m CROSS JOIN generate_series(0, 3) j JOIN keys_n k ON k.n = 1 + (m * 5 + j * 13) % {self.nkeys}
	WHERE m % 7 <> 0 AND j <= m % 4 ON CONFLICT DO NOTHING;
INSERT INTO tenant_scope SELECT m, k.k FROM generate_series(1, {self.dim}) m JOIN keys_n k ON k.n = 1 + (m * 5) % {self.nkeys} WHERE m % 7 <> 0 ON CONFLICT DO NOTHING;
CREATE INDEX tenant_scope_col ON tenant_scope USING sift_col ({mc}, tenant_id) WITH (track_changes = 'ctid');
CREATE TABLE events (id int PRIMARY KEY, dim_a_id int, created_at timestamptz, kind int, v int, pad text{gen(ft)}) WITH (autovacuum_enabled = false);
INSERT INTO events SELECT {self.fact_rows(1, self.rows)};
{self.two_setup(rt, gen)}CREATE INDEX events_col ON events USING sift_col (id, {mc}, created_at, kind, v)
	WITH (scope = '{self.declaration()}');
SET sift.scope_max_multiplicity = 64;
REINDEX INDEX events_col;
SET sift.scope_max_multiplicity = 8;
VACUUM (FREEZE, ANALYZE) events, tenant_scope, keys_n{', grp_scope, memberships' if self.two else ''};
"""

    def declaration(self):
        mc = self.mc
        one = f'tenant_id: {mc} -> tenant_scope({mc}, tenant_id)'
        return one + f' | grp_scope(group_id, tenant_id) via memberships(group_id, {mc})' if self.two else one

    def two_setup(self, rt, gen):
        """The grants of groups, the memberships and the view of the union, with the journals of the three relations."""
        if not self.two:
            return ''
        mc, g = self.mc, self.groups
        uq = f', UNIQUE (group_id, {mc})' if mc == 'account' else ''
        return f"""CREATE TABLE grp_scope (group_id int NOT NULL, tenant_id {self.ktype} NOT NULL, PRIMARY KEY (group_id, tenant_id)) WITH (autovacuum_enabled = false);
CREATE TABLE memberships (group_id int NOT NULL, dim_a_id int NOT NULL{gen(rt).replace('STORED', 'STORED NOT NULL')}, PRIMARY KEY (group_id, dim_a_id){uq}) WITH (autovacuum_enabled = false);
INSERT INTO grp_scope SELECT g, k.k FROM generate_series(1, {g}) g CROSS JOIN generate_series(0, 1) j JOIN keys_n k ON k.n = 1 + (g * 3 + j * 7) % {self.nkeys} WHERE j <= g % 2 ON CONFLICT DO NOTHING;
INSERT INTO memberships (group_id, dim_a_id) SELECT 1 + (m * 3 + j) % {g}, m FROM generate_series(1, {self.dim}) m CROSS JOIN generate_series(0, 1) j WHERE m % 5 <> 0 AND j <= m % 2 ON CONFLICT DO NOTHING;
CREATE INDEX grp_scope_col ON grp_scope USING sift_col (group_id, tenant_id) WITH (track_changes = 'ctid');
CREATE INDEX memberships_col ON memberships USING sift_col ({mc}, group_id) WITH (track_changes = 'ctid');
CREATE VIEW pair_set AS SELECT {mc}, tenant_id FROM tenant_scope UNION SELECT m.{mc}, g.tenant_id FROM grp_scope g JOIN memberships m ON m.group_id = g.group_id;
"""

    def fact_rows(self, a, b):
        return (f"i, CASE WHEN i % 53 = 0 THEN NULL ELSE 1 + (i * 7919) % {self.dim + 20} END, TIMESTAMPTZ '2026-01-01' + (i % 300) * interval '1 day', i % 5, i % 10, 'p' "
                f"FROM generate_series({a}, {b}) i")

    def window(self, pct, col='id'):
        self.off += 1
        return f"(({col} + {self.off}) * 7919 % 100) < {pct}"

    def pair_ins(self, pct, spare=False):
        lo, hi = (self.nkeys, self.nkeys + 40) if spare else (0, self.nkeys)
        return (f"INSERT INTO tenant_scope SELECT m, (SELECT k.k FROM keys_n k WHERE k.n > {lo} AND k.n <= {hi} AND NOT EXISTS (SELECT 1 FROM tenant_scope s WHERE s.dim_a_id = m AND s.tenant_id = k.k) "
                f"ORDER BY (k.n * 31 + m) % 17 LIMIT 1) FROM generate_series(1, {self.dim}) m WHERE {self.window(pct, 'm')} ON CONFLICT DO NOTHING")

    def pair_del(self, pct):
        return f"DELETE FROM tenant_scope WHERE {self.window(pct, 'dim_a_id')} AND tenant_id IN (SELECT k FROM keys_n WHERE n % 2 = {self.rng.randint(0, 1)})"

    def pair_move(self, pct):
        return (f"WITH d AS (DELETE FROM tenant_scope WHERE {self.window(pct, 'dim_a_id')} AND tenant_id IN (SELECT k FROM keys_n WHERE n % 3 = {self.rng.randint(0, 2)}) RETURNING dim_a_id) "
                f"INSERT INTO tenant_scope SELECT DISTINCT d.dim_a_id, k.k FROM d JOIN keys_n k ON k.n = 1 + (d.dim_a_id * 3) % {self.nkeys} ON CONFLICT DO NOTHING")

    def pair_rekey(self, pct):
        return (f"UPDATE tenant_scope t SET tenant_id = nk.k FROM keys_n ok, keys_n nk WHERE ok.k = t.tenant_id AND ok.n <= {self.nkeys} AND nk.n = 1 + ok.n % {self.nkeys} AND {self.window(pct, 't.dim_a_id')} "
                f"AND NOT EXISTS (SELECT 1 FROM tenant_scope s WHERE s.dim_a_id = t.dim_a_id AND s.tenant_id = nk.k)")

    def key_gone(self, pct):
        return f"DELETE FROM tenant_scope WHERE tenant_id = {self.k(self.rng.choice(self.keys))}"

    def grp_ins(self, pct):
        return (f"INSERT INTO grp_scope SELECT g, k.k FROM generate_series(1, {self.groups}) g JOIN keys_n k ON k.n = 1 + (g * 5 + {self.off}) % {self.nkeys + 10} "
                f"WHERE {self.window(pct, 'g')} ON CONFLICT DO NOTHING")

    def grp_del(self, pct):
        return f"DELETE FROM grp_scope WHERE {self.window(pct, 'group_id')}"

    def grp_move(self, pct):
        return (f"WITH d AS (DELETE FROM grp_scope WHERE {self.window(pct, 'group_id')} RETURNING group_id) INSERT INTO grp_scope "
                f"SELECT d.group_id, k.k FROM d JOIN keys_n k ON k.n = 1 + (d.group_id * 7) % {self.nkeys} ON CONFLICT DO NOTHING")

    def mem_ins(self, pct):
        return (f"INSERT INTO memberships (group_id, dim_a_id) SELECT 1 + (m * 3 + {self.off}) % {self.groups}, m FROM generate_series(1, {self.dim + 20}) m "
                f"WHERE {self.window(pct, 'm')} ON CONFLICT DO NOTHING")

    def mem_del(self, pct):
        return f"DELETE FROM memberships WHERE {self.window(pct, 'dim_a_id')}"

    def mem_move(self, pct):
        return (f"WITH d AS (DELETE FROM memberships WHERE {self.window(pct, 'dim_a_id')} RETURNING group_id, dim_a_id) INSERT INTO memberships (group_id, dim_a_id) "
                f"SELECT 1 + (group_id + 1) % {self.groups}, dim_a_id FROM d ON CONFLICT DO NOTHING")

    GROUP_STEPS = [('grp_ins', 6), ('grp_del', 5), ('grp_move', 5), ('mem_ins', 7), ('mem_del', 6), ('mem_move', 6)]
    STEPS = [('ins', 10), ('buffer', 8), ('del', 7), ('upd_val', 7), ('upd_member', 7), ('pair_ins', 11), ('pair_fresh', 8), ('pair_del', 9), ('pair_move', 10), ('pair_rekey', 7),
             ('key_gone', 3), ('aborted', 5), ('savepoints', 5), ('txn_read', 5), ('promote', 7), ('merge', 5), ('vacuum', 6), ('vacuum_pairs', 3), ('reindex', 3), ('hot', 3)]

    def step(self, reader):
        r = self.rng
        steps = self.STEPS + (self.GROUP_STEPS if self.two else [])
        kind = r.choices([k for k, _ in steps], [w for _, w in steps])[0]
        if reader and kind == 'reindex':   # the reader's snapshot holds the index: a rebuild would wait for it
            kind = 'promote'
        pct = r.choice([1, 3, 10, 30, 60])
        if kind == 'ins':
            a, m = self.next_id, r.choice([50, 500, 2500])
            self.next_id += m
            return kind, f"INSERT INTO events SELECT {self.fact_rows(a, a + m - 1)}"
        if kind == 'buffer':
            a, m = self.next_id, r.choice([30, 200])
            self.next_id += m
            return kind, (f"INSERT INTO events SELECT {self.fact_rows(a, a + m - 1)}; UPDATE events SET v = (v + 1) % 10 WHERE {self.window(pct)}; DELETE FROM events WHERE id > {a} AND id % 13 = 0")
        if kind == 'del':
            return kind, f"DELETE FROM events WHERE {self.window(pct)}"
        if kind == 'upd_val':
            return kind, f"UPDATE events SET v = (v + 3) % 10, created_at = created_at + interval '30 hours' WHERE {self.window(pct)}"
        if kind == 'hot':
            return kind, f"UPDATE events SET pad = 'h' WHERE {self.window(pct)}"
        if kind == 'upd_member':
            return kind, f"UPDATE events SET dim_a_id = 1 + (dim_a_id * 7 + id) % {self.dim + 20} WHERE dim_a_id IS NOT NULL AND {self.window(pct)}"
        if kind == 'pair_fresh':
            return kind, self.pair_ins(pct, True)
        if kind in ('pair_ins', 'pair_del', 'pair_move', 'pair_rekey', 'key_gone', 'grp_ins', 'grp_del', 'grp_move', 'mem_ins', 'mem_del', 'mem_move'):
            return kind, getattr(self, kind)(pct)
        if kind == 'aborted':
            return kind, f"BEGIN; DELETE FROM events WHERE {self.window(pct)}; {self.pair_move(pct)}; {self.pair_del(pct)}; {self.group_churn(pct)}ROLLBACK"
        if kind == 'savepoints':
            return kind, (f"BEGIN; SAVEPOINT a; {self.pair_move(pct)}; SAVEPOINT b; DELETE FROM events WHERE {self.window(pct)}; ROLLBACK TO b; {self.pair_ins(pct, True)}; "
                          f"ROLLBACK TO a; {self.pair_rekey(pct)}; SAVEPOINT c; {self.pair_del(pct)}; {self.group_churn(pct)}RELEASE c; COMMIT")
        if kind == 'txn_read':
            return kind, f"BEGIN; {self.pair_move(pct)}; {self.pair_ins(pct, True)}; DELETE FROM events WHERE {self.window(pct)}"
        if kind == 'promote':
            return kind, "SELECT sift.col_promote('events_col')"
        if kind == 'merge':
            return kind, "SELECT sift.col_merge('events_col', true)"
        if kind == 'vacuum':
            return kind, 'VACUUM events'
        if kind == 'vacuum_pairs':
            return kind, 'VACUUM tenant_scope' + (', grp_scope, memberships' if self.two else '')
        return kind, 'SET sift.scope_max_multiplicity = 64; REINDEX INDEX events_col; SET sift.scope_max_multiplicity = 8'

    def group_churn(self, pct):
        return f"{self.mem_move(pct)}; {self.grp_move(pct)}; " if self.two else ''

    def battery(self):
        r = self.rng
        pick = lambda: r.choice(self.keys + self.spare[:3]) if r.random() < 0.85 else r.choice(self.spare)
        a = pick()
        ks = [pick() for _ in range(r.randint(2, 4))] + [self.spare[-1]]
        t = self.ktype
        m1 = r.randint(1, 8)
        mc = self.mc
        P, E = self.pairs, 'events e'
        agg = f"count(*), sum(v), min(id), max(id), count(DISTINCT {mc})"
        return [
            ('one key', f"SELECT {agg} FROM {E} WHERE e.{mc} IN (SELECT {mc} FROM {P} WHERE tenant_id = {self.k(a)})"),
            ('list', f"SELECT {agg} FROM {E} WHERE e.{mc} IN (SELECT {mc} FROM {P} WHERE tenant_id IN ({self.klist(ks)}))"),
            ('exists', f"SELECT kind, count(*), sum(v) FROM {E} WHERE EXISTS (SELECT 1 FROM {P} s WHERE s.{mc} = e.{mc} AND s.tenant_id = ANY (ARRAY[{self.klist(ks)}]::{t}[])) GROUP BY kind"),
            ('per pair', f"SELECT kind, count(*), sum(v) FROM {E} JOIN {P} s ON s.{mc} = e.{mc} WHERE s.tenant_id IN ({self.klist(ks)}) GROUP BY kind"),
            ('group key', f"SELECT s.tenant_id, count(*), sum(v), min(id) FROM {E} JOIN {P} s ON s.{mc} = e.{mc} GROUP BY s.tenant_id"),
            ('group key list', f"SELECT s.tenant_id, kind, count(*) FROM {E} JOIN {P} s ON s.{mc} = e.{mc} WHERE s.tenant_id IN ({self.klist(ks)}) GROUP BY s.tenant_id, kind"),
            ('table of keys', f"SELECT s.tenant_id, count(*), sum(v) FROM {E} JOIN {P} s ON s.{mc} = e.{mc} JOIN keys_n k ON k.k = s.tenant_id WHERE k.n IN ({a}, {pick()}) GROUP BY s.tenant_id"),
            ('time', f"SELECT extract(month FROM created_at) m, count(*), sum(v) FROM {E} WHERE e.{mc} IN (SELECT {mc} FROM {P} WHERE tenant_id = {self.k(a)}) "
                     f"AND created_at >= DATE '2026-0{m1}-01' AND created_at < DATE '2026-0{m1 + 1}-01' + 40 GROUP BY 1"),
            ('top', f"SELECT id, created_at FROM {E} WHERE e.{mc} IN (SELECT {mc} FROM {P} WHERE tenant_id = {self.k(a)}) ORDER BY created_at DESC, id DESC LIMIT 20"),
            ('every key', f"SELECT count(*), sum(v) FROM {E} JOIN {P} s ON s.{mc} = e.{mc}")]


def run_battery(a, qs, stats, label, strict=False, first=None):
    """The rows of every statement are the plain rows; returns {name: hash of the plain rows}."""
    sql = '\n'.join(f"SELECT {i}, tk_cmp({sqlq(q)});" for i, (n, q) in enumerate(qs))
    out = a.run(sql)
    if ERR.search(out):
        raise Fail(f'{label}: {out.strip()[:800]}')
    got = {}
    for line in out.strip().splitlines():
        m = re.match(r'^(\d+)\|(true|false)\|(true|false)\|([^|]*)\|(.*)$', line)
        if not m:
            continue
        i, same, engaged, why, h = int(m[1]), m[2] == 'true', m[3] == 'true', m[4], m[5]
        name, q = qs[i]
        if not same:
            raise Fail(f'{label}: {name}: the scope read differs from the plain plan: {q}')
        if first is not None and first[i] != h:
            raise Fail(f'{label}: {name}: the reader saw other rows than its first read: {q}')
        stats['reads'] += 1
        stats['engaged'] += engaged
        stats['fallback'] += bool(why)
        if why:
            stats['why ' + why] += 1
        if why and not DOCUMENTED.match(why):
            raise Fail(f'{label}: {name}: undocumented fallback reason "{why}"')
        if strict and name == 'one key' and (not engaged or why):
            raise Fail(f'{label}: {name}: not stored right after a build ({why or "no scope read"})')
        got[i] = h
    if len(got) != len(qs):
        raise Fail(f'{label}: {len(got)} of {len(qs)} answers: {out.strip()[:500]}')
    return got


def sqlq(q):
    return "'" + q.replace("'", "''") + "'"


def run_seed(seed, rows, steps, out_dir):
    os.makedirs(out_dir, exist_ok=True)
    path = f'{out_dir}/textkey_{"two_" if TWO else ""}seed_{seed}.log'
    log = open(path, 'w')
    a, rd = Sess('writer', log, FUZZ_SETUP), Sess('reader', log, FUZZ_SETUP)
    stats = collections.Counter()
    try:
        s = Seed(seed, rows)
        log.write(f'-- seed {seed} {vars(s)}\n')
        a.run(LIB)
        out = a.run(s.setup_sql())
        if ERR.search(out):
            raise Fail(f'setup: {out.strip()[:1500]}')
        run_battery(a, s.battery(), stats, 'base', strict=True)
        out = a.run("SELECT 'verify', sift.col_verify('events_col', true);")
        if ERR.search(out):
            raise Fail(f'base: col_verify: {out.strip()[:500]}')
        reader = s.rng.random() < 0.6
        rqs = first = None
        if reader:
            rd.run(LIB)
            rqs = s.battery()
            rd.run('BEGIN ISOLATION LEVEL REPEATABLE READ;')
            first = run_battery(rd, rqs, stats, 'reader first read')
        for i in range(steps):
            kind, sql = s.step(reader)
            label = f'step {i} {kind}'
            out = a.run(sql + ';')
            if ERR.search(out):
                raise Fail(f'{label}: {sql[:300]}: {out.strip()[:800]}')
            run_battery(a, s.battery(), stats, label, strict=kind == 'reindex')
            if kind == 'txn_read':
                a.run(s.rng.choice(['COMMIT;', 'ROLLBACK;']))
            out = a.run("SELECT 'verify', sift.col_verify('events_col', true);")
            if ERR.search(out):
                raise Fail(f'{label}: col_verify: {out.strip()[:500]}')
            if reader:
                run_battery(rd, rqs, stats, f'{label} (reader)', first=first)
        if reader:
            rd.run('COMMIT;')
        a.run('VACUUM events;')
        run_battery(a, s.battery(), stats, 'final')
        out = a.run('SET sift.scope_max_multiplicity = 64; REINDEX INDEX events_col; SET sift.scope_max_multiplicity = 8;')   # churn raises the entries of a row above the limit of the build
        if ERR.search(out):
            raise Fail(f'final rebuild: {out.strip()[:500]}')
        run_battery(a, s.battery(), stats, 'rebuilt', strict=True)
        if not stats['engaged']:
            raise Fail('no read of the seed took the scope units')
        why = ', '.join(f'{k[4:]} x{v}' for k, v in stats.most_common() if k.startswith('why '))
        return 0, path, f"{'two paths, ' if s.two else ''}{s.ktype} key, {s.mode} member, reads={stats['reads']} scope={stats['engaged']} fallback={stats['fallback']} ({why})"
    except Fail as e:
        log.write(f'FAIL {e}\n')
        return 1, path, f'FAIL {str(e)[:600]}'
    finally:
        a.close()
        rd.close()
        log.close()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--seeds', default='1-3')
    ap.add_argument('--rows', type=int, default=8000)
    ap.add_argument('--steps', type=int, default=8)
    ap.add_argument('--two-paths', action='store_true', help='a scope of two paths: the direct pairs and the pairs through the groups a key is granted')
    ap.add_argument('--hash-bits', type=int, default=0, help='sift.test_member_hash_bits: the low bits kept of the journal word of a string member (string values collide)')
    o = ap.parse_args()
    global FUZZ_SETUP, TWO
    TWO = o.two_paths
    if o.hash_bits:
        FUZZ_SETUP += f' SET sift.test_member_hash_bits = {o.hash_bits};'
    lo, _, hi = o.seeds.partition('-')
    bad = 0
    for seed in range(int(lo), int(hi or lo) + 1):
        t0 = time.time()
        rc, path, note = run_seed(seed, o.rows, o.steps, OUT)
        print(f'seed={seed} {"OK" if rc == 0 else "FAIL"} {time.time() - t0:.0f}s {note} log={path}', flush=True)
        bad += rc
    sys.exit(1 if bad else 0)


if __name__ == '__main__':
    main()
