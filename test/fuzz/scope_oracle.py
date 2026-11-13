#!/usr/bin/env python3
"""Oracle gate of the scope index: random workloads whose expected answer comes from stock PostgreSQL only (test/t/col_scope_oracle.sh runs it).

  test/fuzz/scope_oracle.py --primary SOCK:PORT --standby SOCK:PORT --stock SOCK:PORT --restore SOCK:PORT --out DIR [--seeds 1-3] [--rows 60000] [--steps 8] [--window 6]

The nodes: PRIMARY has pg_sift and the scope index (wal_level logical); STANDBY streams from it (physical, hot standby); STOCK never loads pg_sift (a cluster that holds the plain tables only, fed by logical replication of the base
tables: the oracle, no sift code on its side); RESTORE has pg_sift and takes a dump of the primary.

SIFT_ORACLE_SET is SQL every session of the primary runs first (`SET sift.test_no_track = on;` is a mutant of the tracking that the gate must catch).

A seed draws a shape (one path, soft deleted pairs, a scope of two paths), a size and builds the facts and the pairs. Steps change facts and pairs (inserts, updates, moves of the member, HOT updates, deletes, upserts; pairs
inserted, deleted, moved, reloaded, soft deleted, restored; grants and memberships), run VACUUM, promotion, merge, REINDEX (also CONCURRENTLY) and CREATE INDEX CONCURRENTLY of a second scope index, leave a writer's transaction open
while the others read, roll back with savepoints, and read inside the transaction that edited (own command ids). After a step the battery of reads (one key, sets of keys counted once, per pair, per key, top-N, EXISTS) runs:
  - on the primary through the scope units, through the planned plan and through the plain plan (sift disabled, no index used), in ONE REPEATABLE READ snapshot: the three must be equal;
  - on the STOCK node (once its subscription has caught up), on the STANDBY (once it replayed) and on the RESTORE node: each must equal the primary's plain answer, row for row.
Windows of concurrent writers (pgbench) run with REPEATABLE READ and SERIALIZABLE readers on the primary and the standby that each compare the scope read with the plain plan inside their snapshot and with the answer
of the first read of that snapshot; a CREATE INDEX CONCURRENTLY runs inside one of them. At the end of a seed sift.col_scope_verify must find no differing key on the primary and the standby, and the dump of the primary is
restored and read. The first wrong answer stops the run (exit 1) with the query, the arms and the first differing rows in OUT/FAIL-*.txt. Statistics (reads compared, reads that used the scope units) end the run.
"""
import argparse, os, random, re, subprocess, sys, time

ERR = re.compile(r'^(ERROR|FATAL|PANIC|server closed|connection to server|psql: error)', re.M)
TZ = "SET TimeZone = 'UTC'; SET client_min_messages = warning; SET max_parallel_workers_per_gather = 0; SET jit = off; SET lock_timeout = '60s';"
WHY = {}
STATS = {'compared': 0, 'scope_reads': 0, 'engaged': 0, 'windows': 0, 'own': 0, 'snapshots': 0, 'verify': 0, 'unverified': 0}


class Fail(Exception):
    pass


class Pg:
    """A psql session on a node: send a script, read to the sentinel."""
    ALL = []

    def __init__(self, name, node, db, log, setup='', sift=True):
        host, port = node.rsplit(':', 1)
        self.name, self.log, self.n, self.node, self.db = name, log, 0, node, db
        Pg.ALL.append(self)
        env = dict(os.environ, LC_ALL='C', PGHOST=host, PGPORT=port)
        for k in ('PGDATABASE', 'PGSERVICE', 'PGOPTIONS'):
            env.pop(k, None)
        self.p = subprocess.Popen(['psql', '-X', '-A', '-t', '-F', '|', '-U', 'postgres', '-d', db], stdin=subprocess.PIPE,
                                  stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, env=env)
        self.run('\\set ON_ERROR_ROLLBACK on\n' + ("LOAD 'pg_sift';" if sift else '') + TZ + setup)

    def run(self, sql, allow=None):
        self.n += 1
        sql = sql.rstrip()
        if not sql.endswith(';'):
            sql += ';'
        tok = f'@@END {self.n}'
        self.log.write(f'-- {self.name}\n{sql.strip()}\n')
        self.p.stdin.write(sql.rstrip() + f'\n\\echo {tok}\n')
        self.p.stdin.flush()
        out = []
        while True:
            line = self.p.stdout.readline()
            if not line:
                raise Fail(f'{self.name}: psql exited: {"".join(out)[-800:]}')
            if line.strip() == tok:
                break
            out.append(line)
        out = ''.join(out)
        self.log.write(out)
        self.log.flush()
        errs = [m.group(0) + out[m.end():].split('\n', 1)[0] for m in ERR.finditer(out)]
        if errs and not (allow and all(re.search(allow, e) for e in errs)):
            raise Fail(f'{self.name}: {errs[0]}')
        return out

    def val(self, sql):
        return self.run(sql).strip().split('\n')[-1]

    def close(self):
        try:
            self.p.stdin.close()
            self.p.wait(timeout=10)
        except Exception:
            self.p.kill()


def shell(cmd, env=None, check=True):
    r = subprocess.run(cmd, shell=True, capture_output=True, text=True, env=dict(os.environ, **(env or {})))
    if check and r.returncode:
        raise Fail(f'{cmd[:200]}: {r.stderr[-800:]}')
    return r.stdout


# ---------------------------------------------------------------------------------------------------------------------------------------------------------------- shapes

class Shape:
    """The tables, the scope declaration, the reads in their plain form and the edits of the pairs of one shape."""

    def __init__(self, name, rng, n_members, tenants, walk=False):
        self.name, self.rng, self.M, self.T, self.walk = name, rng, n_members, tenants, walk
        self.G = max(6, n_members // 10)
        self.pad = rng.choice([800, 1500, 2400]) if walk else 0
        self.icols = rng.choice(['id, dim_a_id, created_at', 'id, dim_a_id, created_at, kind, amount'])
        # a time series (created_at grows with the id, as in an append-only table) or a cyclic one
        self.created = ("timestamptz '2026-01-01' + i * interval '40 seconds'" if rng.random() < 0.5
                        else "timestamptz '2026-01-01' + (i % 400) * interval '1 hour' + (i % 3600) * interval '1 second'")
        if walk:   # the rows of a member are dated around a date of its own, so the units cut by member have bounds that differ; wide rows make many superblocks
            self.created = (f"timestamptz '2026-01-01' + (hashint4(1 + i % {n_members}) & 2147483647) % 40 * interval '30 days' "
                            "+ (hashint4(i) & 2147483647) % 20000 * interval '1 minute'")

    # the SQL every node runs: tables and the plain btrees (no sift)
    def base_ddl(self):
        pad = f" DEFAULT repeat('x', {self.pad})" if self.walk else ''
        f = ("CREATE TABLE marks (n int PRIMARY KEY); CREATE TABLE facts (id int PRIMARY KEY, dim_a_id int, created_at timestamptz NOT NULL, kind int NOT NULL, amount int NOT NULL, pad text" + pad + ") "
             f"WITH ({'' if self.walk else 'fillfactor = 70, '}autovacuum_enabled = false);" + ("ALTER TABLE facts ALTER COLUMN pad SET STORAGE PLAIN;" if self.walk else ''))
        if self.name == 'plain':
            return f + ("CREATE TABLE pairs (dim_a_id int NOT NULL, tenant_id int NOT NULL, note text, PRIMARY KEY (dim_a_id, tenant_id)) WITH (fillfactor = 70, autovacuum_enabled = false);")
        if self.name == 'soft':
            return f + ("CREATE TABLE pairs (dim_a_id int NOT NULL, tenant_id int NOT NULL, deleted_at timestamptz, note text, PRIMARY KEY (dim_a_id, tenant_id)) WITH (fillfactor = 70, autovacuum_enabled = false);"
                        "CREATE UNIQUE INDEX pairs_active ON pairs (dim_a_id, tenant_id) WHERE deleted_at IS NULL;")
        return f + ("CREATE TABLE grants (id serial PRIMARY KEY, tenant_id int NOT NULL, dim_a_id int, group_id int, deleted_at timestamptz, note text) WITH (fillfactor = 70, autovacuum_enabled = false);"
                    "CREATE UNIQUE INDEX grants_direct ON grants (dim_a_id, tenant_id) WHERE deleted_at IS NULL;"
                    "CREATE UNIQUE INDEX grants_group ON grants (group_id, tenant_id) WHERE deleted_at IS NULL;"
                    "CREATE TABLE group_members (id serial PRIMARY KEY, group_id int NOT NULL, dim_a_id int NOT NULL, deleted_at timestamptz, note text) WITH (fillfactor = 70, autovacuum_enabled = false);"
                    "CREATE UNIQUE INDEX members_active ON group_members (group_id, dim_a_id) WHERE deleted_at IS NULL;")

    def tables(self):
        return ['marks', 'facts', 'pairs'] if self.name != 'union' else ['marks', 'facts', 'grants', 'group_members']

    def populate(self, n):
        M, T, G = self.M, self.T, self.G
        s = (f"INSERT INTO facts (id, dim_a_id, created_at, kind, amount{'' if self.walk else ', pad'}) SELECT i, {('1 + i % ' + str(M)) if self.walk else f'1 + (i::bigint * 7919 % {M + 20})::int'}, {self.created}, "
             f"i % 7, (i * 13) % 1000{'' if self.walk else ", 'x'"} FROM generate_series(1, {n}) i;")
        if self.walk:   # 200 member ids, each in the group of one of 10 tenants (one tenant is a tenth of the table)
            if self.name == 'union':
                s += (f"INSERT INTO group_members (group_id, dim_a_id) SELECT 1 + m % {T}, m FROM generate_series(1, {M}) m; "
                      f"INSERT INTO grants (tenant_id, group_id) SELECT t, t FROM generate_series(1, {T}) t;")
            else:
                s += f"INSERT INTO pairs (dim_a_id, tenant_id) SELECT m, 1 + m % {T} FROM generate_series(1, {M}) m;"
        elif self.name == 'plain':
            s += f"INSERT INTO pairs SELECT m, 1 + (m * 31 + j * 13) % {T} FROM generate_series(1, {M}) m, generate_series(0, 5) j WHERE j < m % 6 ON CONFLICT DO NOTHING;"
        elif self.name == 'soft':
            s += (f"INSERT INTO pairs SELECT m, 1 + (m * 31 + j * 13) % {T}, CASE WHEN (m + j) % 7 = 0 THEN now() END FROM generate_series(1, {M}) m, "
                  f"generate_series(0, 5) j WHERE j < m % 6 ON CONFLICT DO NOTHING;")
        else:
            s += (f"INSERT INTO group_members (group_id, dim_a_id) SELECT 1 + (m - 1) % {G}, m FROM generate_series(1, {M}) m;"
                  f"INSERT INTO group_members (group_id, dim_a_id) SELECT 1 + (m * 3) % {G}, m FROM generate_series(1, {M}) m WHERE m % 4 = 0 ON CONFLICT DO NOTHING;"
                  f"INSERT INTO grants (tenant_id, group_id) SELECT 1 + (g * 5) % {T}, g FROM generate_series(1, {G}) g ON CONFLICT DO NOTHING;"
                  f"INSERT INTO grants (tenant_id, group_id) SELECT 1 + (g * 11) % {T}, g FROM generate_series(1, {G}) g WHERE g % 2 = 0 ON CONFLICT DO NOTHING;"
                  f"INSERT INTO grants (tenant_id, dim_a_id) SELECT 1 + (m * 7) % {T}, m FROM generate_series(1, {M}) m WHERE m % 5 = 0 ON CONFLICT DO NOTHING;"
                  f"UPDATE grants SET deleted_at = now() WHERE id % 13 = 0; UPDATE group_members SET deleted_at = now() WHERE id % 17 = 0;")
        return s

    # the sift DDL of the primary
    def sift_ddl(self, opts):
        j = "CREATE INDEX {n} ON {t} USING sift_col ({c}) WITH (track_changes = 'ctid');"
        if self.name == 'union':
            d = (j.format(n='grants_by_member', t='grants', c='dim_a_id, tenant_id') + j.format(n='grants_by_group', t='grants', c='group_id, tenant_id')
                 + j.format(n='members_by_member', t='group_members', c='dim_a_id, group_id'))
        else:
            d = j.format(n='pairs_journal', t='pairs', c='dim_a_id, tenant_id')
        return d + self.scope_index('facts_col', opts)

    def scope_clause(self):
        if self.name == 'union':
            return 'tenant_id: dim_a_id -> grants(dim_a_id, tenant_id) | grants(group_id, tenant_id) via group_members(group_id, dim_a_id)'
        return 'tenant_id: dim_a_id -> pairs(dim_a_id, tenant_id)'

    def scope_index(self, name, opts, concurrently=False):
        c = ' CONCURRENTLY' if concurrently else ''
        return f"CREATE INDEX{c} {name} ON facts USING sift_col ({self.icols}) WITH (scope = '{self.scope_clause()}'{opts});"

    # the member ids of the keys in the plain form ({k}: a comma list of keys)
    def scope_members(self, k):
        if self.name == 'plain':
            return f"SELECT dim_a_id FROM pairs WHERE tenant_id IN ({k})"
        if self.name == 'soft':
            return f"SELECT dim_a_id FROM pairs WHERE tenant_id IN ({k}) AND deleted_at IS NULL"
        return (f"SELECT dim_a_id FROM grants WHERE tenant_id IN ({k}) AND deleted_at IS NULL "
                f"UNION SELECT m.dim_a_id FROM grants g JOIN group_members m ON m.group_id = g.group_id WHERE g.tenant_id IN ({k}) AND g.deleted_at IS NULL AND m.deleted_at IS NULL")

    def pairs_from(self):
        """FROM and WHERE fragments of a join per pair: the pairs as (dim_a_id, tenant_id)."""
        if self.name == 'plain':
            return "pairs p ON p.dim_a_id = f.dim_a_id", ""
        if self.name == 'soft':
            return "pairs p ON p.dim_a_id = f.dim_a_id", " AND p.deleted_at IS NULL"
        return ("(SELECT dim_a_id, tenant_id FROM grants WHERE deleted_at IS NULL AND dim_a_id IS NOT NULL UNION SELECT m.dim_a_id, g.tenant_id FROM grants g "
                "JOIN group_members m ON m.group_id = g.group_id WHERE g.deleted_at IS NULL AND m.deleted_at IS NULL) p ON p.dim_a_id = f.dim_a_id"), ""

    def queries(self, rng, ntenants):
        """The battery: (name, sql) with constant keys drawn now."""
        T = ntenants
        k1, ks = rng.randint(1, T), sorted(rng.sample(range(1, T + 1), 3))
        K = ', '.join(map(str, ks))
        jf, jw = self.pairs_from()
        top = rng.choice(['DESC', 'ASC'])
        every = [(f'latest_t{t}', f"SELECT f.id, f.created_at FROM facts f WHERE f.dim_a_id IN ({self.scope_members(t)}) ORDER BY f.created_at DESC, f.id DESC LIMIT 5")
                 for t in range(1, T + 1)] if self.walk else []
        return every + [
            ('one_key', f"SELECT f.id, f.dim_a_id, f.kind, f.amount, f.created_at FROM facts f WHERE f.dim_a_id IN ({self.scope_members(k1)}) ORDER BY f.id"),
            ('keys_once', f"SELECT f.id, f.amount FROM facts f WHERE f.dim_a_id IN ({self.scope_members(K)}) ORDER BY f.id"),
            ('exists_keys', f"SELECT f.kind, count(*), sum(f.amount), min(f.id), max(f.id) FROM facts f WHERE EXISTS "
                            f"(SELECT 1 FROM ({self.scope_members(K)}) mm WHERE mm.dim_a_id = f.dim_a_id) GROUP BY 1 ORDER BY 1"),
            ('per_pair_key', f"SELECT count(*), sum(f.amount), sum(f.id) FROM facts f JOIN {jf} WHERE p.tenant_id = {k1}{jw}"),
            ('per_pair_keys', f"SELECT p.tenant_id, count(*), sum(f.amount), min(f.id), max(f.id) FROM facts f JOIN {jf} WHERE p.tenant_id IN ({K}){jw} GROUP BY 1 ORDER BY 1"),
            ('per_member', f"SELECT f.dim_a_id, count(*), sum(f.amount), min(f.id) FROM facts f WHERE f.dim_a_id IN ({self.scope_members(K)}) GROUP BY 1 ORDER BY 1"),
            ('first_ids', f"SELECT f.id, f.dim_a_id, f.amount FROM facts f WHERE f.dim_a_id IN ({self.scope_members(k1)}) ORDER BY f.id LIMIT 400"),
            ('every_key', f"SELECT p.tenant_id, count(*), sum(f.amount), count(DISTINCT f.dim_a_id) FROM facts f JOIN {jf} WHERE true{jw} GROUP BY 1 ORDER BY 1"),
            ('top_n', f"SELECT f.id, f.created_at FROM facts f WHERE f.dim_a_id IN ({self.scope_members(K)}) ORDER BY f.created_at {top}, f.id {top} LIMIT 25"),
            ('latest_5_key', f"SELECT f.id, f.created_at FROM facts f WHERE f.dim_a_id IN ({self.scope_members(k1)}) ORDER BY f.created_at DESC, f.id DESC LIMIT 5"),
            ('top_desc_5', f"SELECT f.id, f.created_at FROM facts f WHERE f.dim_a_id IN ({self.scope_members(K)}) ORDER BY f.created_at DESC, f.id DESC LIMIT 5"),
            ('top_desc_200_key', f"SELECT f.id, f.created_at, f.amount FROM facts f WHERE f.dim_a_id IN ({self.scope_members(k1)}) ORDER BY f.created_at DESC, f.id DESC LIMIT 200"),
            ('top_asc_50', f"SELECT f.id, f.created_at FROM facts f WHERE f.dim_a_id IN ({self.scope_members(K)}) ORDER BY f.created_at ASC, f.id ASC LIMIT 50"),
            ('top_join_desc', f"SELECT f.id, f.created_at, p.tenant_id FROM facts f JOIN {jf} WHERE p.tenant_id = {k1}{jw} ORDER BY f.created_at DESC, f.id DESC LIMIT 30"),
            ('aggregates', f"SELECT count(*), sum(f.amount), count(DISTINCT f.dim_a_id), min(f.created_at), max(f.created_at) FROM facts f WHERE f.dim_a_id IN ({self.scope_members(k1)})"),
            ('no_key', f"SELECT count(*) FROM facts f WHERE f.dim_a_id IN ({self.scope_members(str(T + 50))})"),
        ]

    # edits of the pairs: the SQL of one step, with a drawn seed for random()
    def pair_edit(self, r):
        M, T, G = self.M, self.T, self.G
        a = r.randint(1, M)
        b = min(M, a + r.randint(5, max(6, M // 8)))
        k, k2 = r.randint(1, T), r.randint(1, T)
        n = r.randint(5, max(6, M // 6))
        sd = f"SELECT setseed({r.random() * 2 - 1:.5f});"
        if self.name == 'plain':
            return sd + r.choice([
                f"INSERT INTO pairs SELECT 1 + (random() * {M})::int, 1 + (random() * {T})::int FROM generate_series(1, {n}) ON CONFLICT DO NOTHING;",
                f"DELETE FROM pairs WHERE tenant_id = {k} AND dim_a_id % 5 = {r.randint(0, 4)};",
                f"WITH d AS (DELETE FROM pairs WHERE tenant_id = {k} AND dim_a_id % 7 = {r.randint(0, 6)} RETURNING dim_a_id) INSERT INTO pairs SELECT dim_a_id, {k2} FROM d ON CONFLICT DO NOTHING;",
                f"UPDATE pairs p SET tenant_id = {k2} WHERE p.tenant_id = {k} AND p.dim_a_id % 11 = {r.randint(0, 10)} AND NOT EXISTS (SELECT 1 FROM pairs q WHERE q.dim_a_id = p.dim_a_id AND q.tenant_id = {k2});",
                f"DELETE FROM pairs WHERE dim_a_id BETWEEN {a} AND {b}; INSERT INTO pairs SELECT m, 1 + (m * {r.randint(3, 40)} + j) % {T} FROM generate_series({a}, {b}) m, "
                f"generate_series(0, 4) j WHERE j < (m + {r.randint(0, 5)}) % 6 ON CONFLICT DO NOTHING;",
                f"DELETE FROM pairs WHERE dim_a_id = {a}; INSERT INTO pairs VALUES ({a}, {k}), ({a}, {k2}) ON CONFLICT DO NOTHING;",
                f"UPDATE pairs SET note = 'h' || dim_a_id WHERE dim_a_id BETWEEN {a} AND {b}; DELETE FROM pairs WHERE dim_a_id BETWEEN {a} AND {b} AND tenant_id = {k};",
                f"UPDATE pairs SET note = 'g' WHERE tenant_id = {k}; UPDATE pairs SET note = 'g2' WHERE tenant_id = {k};"])
        if self.name == 'soft':
            return sd + r.choice([
                f"INSERT INTO pairs SELECT 1 + (random() * {M})::int, 1 + (random() * {T})::int, NULL FROM generate_series(1, {n}) ON CONFLICT (dim_a_id, tenant_id) DO UPDATE SET deleted_at = NULL;",
                f"UPDATE pairs SET deleted_at = now() WHERE tenant_id = {k} AND dim_a_id % 5 = {r.randint(0, 4)} AND deleted_at IS NULL;",
                f"UPDATE pairs SET deleted_at = NULL WHERE deleted_at IS NOT NULL AND tenant_id IN ({k}, {k2}) AND dim_a_id % 3 = {r.randint(0, 2)};",
                f"DELETE FROM pairs WHERE tenant_id = {k} AND dim_a_id % 7 = {r.randint(0, 6)};",
                f"UPDATE pairs SET deleted_at = now() WHERE dim_a_id BETWEEN {a} AND {b} AND deleted_at IS NULL; "
                f"INSERT INTO pairs SELECT m, {k}, NULL FROM generate_series({a}, {b}) m WHERE m % 2 = 0 ON CONFLICT (dim_a_id, tenant_id) DO UPDATE SET deleted_at = NULL;",
                f"UPDATE pairs SET note = 'h' || dim_a_id WHERE dim_a_id BETWEEN {a} AND {b}; DELETE FROM pairs WHERE dim_a_id BETWEEN {a} AND {b} AND tenant_id = {k};",
                f"UPDATE pairs SET note = 'g' WHERE tenant_id = {k}; UPDATE pairs SET note = 'g2' WHERE tenant_id = {k}; UPDATE pairs SET deleted_at = now() WHERE tenant_id = {k} AND dim_a_id % 6 = 0 AND deleted_at IS NULL;"])
        g, g2 = r.randint(1, G), r.randint(1, G)
        return sd + r.choice([
            f"INSERT INTO grants (tenant_id, dim_a_id) SELECT 1 + (random() * {T})::int, 1 + (random() * {M})::int FROM generate_series(1, {max(2, n // 12)}) ON CONFLICT DO NOTHING;",
            f"INSERT INTO grants (tenant_id, group_id) SELECT 1 + (random() * {T})::int, 1 + (random() * {G})::int FROM generate_series(1, {max(2, n // 24)}) ON CONFLICT DO NOTHING;",
            f"UPDATE grants SET deleted_at = now() WHERE tenant_id = {k} AND deleted_at IS NULL AND id % 3 = {r.randint(0, 2)};",
            f"UPDATE grants SET deleted_at = NULL WHERE id IN (SELECT min(id) FROM grants WHERE deleted_at IS NOT NULL AND tenant_id = {k} AND dim_a_id IS NOT NULL GROUP BY dim_a_id HAVING NOT bool_or(deleted_at IS NULL));",
            f"DELETE FROM grants WHERE tenant_id = {k2} AND id % 4 = {r.randint(0, 3)};",
            f"WITH d AS (DELETE FROM grants WHERE dim_a_id = {a} AND deleted_at IS NULL RETURNING tenant_id) INSERT INTO grants (tenant_id, group_id) SELECT tenant_id, {g} FROM d ON CONFLICT DO NOTHING;",
            f"INSERT INTO group_members (group_id, dim_a_id) SELECT 1 + (random() * {G})::int, 1 + (random() * {M})::int FROM generate_series(1, {max(2, n // 6)}) ON CONFLICT DO NOTHING;",
            f"UPDATE group_members SET deleted_at = now() WHERE group_id = {g} AND deleted_at IS NULL AND dim_a_id % 2 = {r.randint(0, 1)};",
            f"UPDATE group_members SET deleted_at = NULL WHERE id IN (SELECT min(id) FROM group_members WHERE deleted_at IS NOT NULL AND group_id = {g2} GROUP BY dim_a_id HAVING NOT bool_or(deleted_at IS NULL));",
            f"WITH d AS (DELETE FROM group_members WHERE group_id = {g} AND dim_a_id % 3 = 0 AND deleted_at IS NULL RETURNING dim_a_id) INSERT INTO group_members (group_id, dim_a_id) SELECT {g2}, dim_a_id FROM d ON CONFLICT DO NOTHING;",
            f"DELETE FROM group_members WHERE group_id = {g} AND dim_a_id % 5 = {r.randint(0, 4)};",
            f"UPDATE grants SET note = 'h' || id WHERE tenant_id = {k}; UPDATE group_members SET note = 'h' || id WHERE group_id = {g}; DELETE FROM grants WHERE tenant_id = {k} AND id % 3 = 0; DELETE FROM group_members WHERE group_id = {g} AND id % 4 = 0;"])


EDIT_OK = r'duplicate key value|cannot affect row a second time|deadlock detected|could not serialize|null value in column'


def edit(pg, sql):
    pg.run(sql, allow=EDIT_OK)


def fact_edit(r, M, state):
    """One edit of the facts; state['next'] is the next id."""
    n = r.randint(20, 400)
    lo = r.randint(1, max(2, state['next'] - 500))
    hi = lo + r.randint(10, 600)
    nid = state['next']
    sd = f"SELECT setseed({r.random() * 2 - 1:.5f});"
    c = r.choice(['ins', 'amount', 'move', 'hot', 'del', 'upsert', 'created'])
    if c == 'ins':
        state['next'] += n
        return sd + (f"INSERT INTO facts SELECT {nid} + i, 1 + (random() * {M + 20})::int, timestamptz '2026-03-01' + (random() * 3000)::int * interval '1 minute', "
                     f"i % 7, (random() * 999)::int, 'n' FROM generate_series(1, {n}) i;")
    if c == 'amount':
        return f"UPDATE facts SET amount = amount + 1 WHERE id BETWEEN {lo} AND {hi};"
    if c == 'move':
        return sd + f"UPDATE facts SET dim_a_id = 1 + (random() * {M + 20})::int WHERE id BETWEEN {lo} AND {hi} AND id % 3 = {r.randint(0, 2)};"
    if c == 'hot':
        return f"UPDATE facts SET pad = 'h' || id WHERE id BETWEEN {lo} AND {hi};"
    if c == 'del':
        return f"DELETE FROM facts WHERE id BETWEEN {lo} AND {hi} AND id % 4 = {r.randint(0, 3)};"
    if c == 'created':
        return f"UPDATE facts SET created_at = created_at + interval '3 days' WHERE id BETWEEN {lo} AND {hi};"
    state['next'] += n
    return sd + (f"INSERT INTO facts SELECT {lo} + i * 3, 1 + (random() * {M + 20})::int, now(), i % 7, i % 1000, 'u' FROM generate_series(1, {n}) i "
                 f"ON CONFLICT (id) DO UPDATE SET amount = facts.amount + 7, dim_a_id = excluded.dim_a_id;")


# --------------------------------------------------------------------------------------------------------------------------------------------------------------- reads

ARMS = {
    'scope':   "SET LOCAL sift.enable_agg = on; SET LOCAL sift.agg_force = on; SET LOCAL sift.enable_col_scan = on;",
    'planned': "SET LOCAL sift.enable_agg = on; SET LOCAL sift.agg_force = off; SET LOCAL sift.enable_col_scan = on;",
    'plain':   "SET LOCAL sift.enable_agg = off; SET LOCAL sift.enable_col_scan = off; SET LOCAL enable_indexscan = off; SET LOCAL enable_bitmapscan = off; "
               "SET LOCAL enable_indexonlyscan = off;",
    'stock':   "",
    'session': "",   # the settings were made for the session by the caller (the toggle check)
}


def digest(q):
    return (f"SELECT md5(coalesce(string_agg(t::text, E'\\n' ORDER BY ord), '')) || '/' || count(*) FROM "
            f"(SELECT row_number() OVER () AS ord, r.* FROM ({q}) r) t")


def answers(pg, arm, qs, snapshot=True, level='REPEATABLE READ'):
    """{name: 'md5/rows'} of the battery under the arm, in one transaction (the caller's when snapshot is False)."""
    s = (f"BEGIN ISOLATION LEVEL {level};" if snapshot else '') + ARMS[arm]
    for name, q in qs:
        s += f"\nSELECT '{name}', ({digest(q)});"
    if snapshot:
        s += "\nCOMMIT;"
    out = pg.run(s)
    res = {m.group(1): m.group(2) for m in re.finditer(r'^(\w+)\|(\S+)$', out, re.M)}
    if len(res) != len(qs):
        raise Fail(f'{pg.name}: battery under {arm} gave {len(res)} of {len(qs)} answers: {out[-400:]}')
    return res


def rows_of(pg, arm, q, intx):
    s = ('' if intx else 'BEGIN ISOLATION LEVEL REPEATABLE READ;') + ARMS[arm] + f"\n\\echo @@ROWS\nSELECT * FROM ({q}) r;\n\\echo @@STOP\n" + ('' if intx else 'COMMIT;')
    out = pg.run(s)
    return out.split('@@ROWS\n', 1)[1].split('@@STOP', 1)[0].strip().split('\n')


class Gate:
    def __init__(self, a):
        self.a, self.out = a, a.out
        self.log = open(os.path.join(a.out, 'oracle.log'), 'a')

    def fail(self, seed, step, what, name, q, details):
        path = os.path.join(self.out, f'FAIL-seed{seed}-{step}.txt')
        with open(path, 'w') as f:
            f.write(f'seed {seed} step {step}: {what}: query {name}\n{q}\n\n{details}\n')
        raise Fail(f'seed {seed} step {step}: {what}: {name} (see {path})')

    def compare(self, seed, step, label, qs, got, want, pg, arm, wpg, warm, intx=False):
        for name, q in qs:
            STATS['compared'] += 1
            if got[name] != want[name]:
                a, b = rows_of(pg, arm, q, intx), rows_of(wpg, warm, q, intx)
                only_a = [x for x in a if x not in set(b)][:8]
                only_b = [x for x in b if x not in set(a)][:8]
                self.fail(seed, step, f'WRONG ANSWER {label}: {arm}={got[name]} {warm}={want[name]}', name, q,
                          f'rows only in {arm} ({len(a)} rows): {only_a}\nrows only in {warm} ({len(b)} rows): {only_b}')


# ---------------------------------------------------------------------------------------------------------------------------------------------------------------- run

def wait_standby(p, s, lsn=None):
    lsn = lsn or p.val("SELECT pg_current_wal_lsn()")
    for _ in range(1200):
        if s.val(f"SELECT pg_last_wal_replay_lsn() >= '{lsn}'") == 't':
            return
        time.sleep(0.1)
    raise Fail('the standby did not replay')


MARK = [0]


def drop_slot(p, name):
    for _ in range(100):
        if 'is active' not in p.run(f"SELECT pg_drop_replication_slot('{name}');", allow='is active'):
            return
        time.sleep(0.2)
    raise Fail(f'slot {name} stays active')


def wait_stock(p, *subs):
    """The subscribers have applied everything committed on the primary: a marker row, committed last, arrives."""
    MARK[0] += 1
    p.run(f"INSERT INTO marks VALUES ({MARK[0]});")
    for sk in subs:
        for _ in range(1200):
            if sk.val(f"SELECT count(*) FROM marks WHERE n = {MARK[0]}") == '1':
                break
            time.sleep(0.1)
        else:
            raise Fail(f'the subscription of {sk.name} did not catch up')


def run_seed(a, seed):
    rng = random.Random(seed)
    db = f'oracle{seed}'
    walk = seed % 2 == 1   # wide rows (about 5 to a page), rows dated by member, units cut by member: the top-N reads walk the superblocks by their zones
    rows = min(a.rows, rng.choice([20000, 40000])) if walk else a.rows
    shape = Shape(['plain', 'soft', 'union'][seed % 3], rng, max(60, rows // 25) if not walk else rng.choice([100, 200, 400]), rng.choice([10, 20]) if walk else 40, walk)
    opts = rng.choice(['', ", write_buffer_limit = 64", ", key_cut = 'dim_a_id'", ", write_buffer_limit = 1024"])
    if walk:
        opts = ", key_order = 'dim_a_id, created_at'"
    patch = rng.choice(['on', 'off'])
    seg = rng.choice([0, 64, 256])
    out = a.out
    log = Gate(a).log
    P, S, B, R = (a.primary, a.standby, a.stock, a.restore)
    root = Pg('admin', P, 'postgres', log, sift=False)
    for n in (P, B, R):
        Pg('admin', n, 'postgres', log, sift=False).run(f"DROP DATABASE IF EXISTS {db} WITH (FORCE); CREATE DATABASE {db};")
    setup = (f"SET sift.test_scope_patch = {patch}; SET sift.scope_max_multiplicity = 8;" + (f" SET sift.test_col_segment_kb = {seg};" if seg else '')
             + os.environ.get('SIFT_ORACLE_SET', ''))
    shell(f"echo 'seed {seed}: shape {shape.name} rows {rows} index options [{opts}] test_scope_patch {patch} segment_kb {seg}' >> {out}/summary.txt")
    print(f'seed {seed}: shape {shape.name}, {rows} rows, options [{opts}], patch {patch}, segment_kb {seg}', flush=True)

    # the base tables on the primary (with data) and on the stock node; the subscription copies the data
    A = Pg('primary', P, db, log, "CREATE EXTENSION IF NOT EXISTS pg_sift; " + setup)
    SK = Pg('stock', B, db, log, sift=False)
    dbl = f'oraclel{seed}'
    Pg('admin', R, 'postgres', log, sift=False).run(f"DROP DATABASE IF EXISTS {dbl} WITH (FORCE); CREATE DATABASE {dbl};")
    LP = Pg('logical-sift', R, dbl, log, "CREATE EXTENSION IF NOT EXISTS pg_sift; " + setup)
    A.run(shape.base_ddl() + shape.populate(rows))
    SK.run(shape.base_ddl())
    tabs = ', '.join(shape.tables())
    A.run(f"CREATE PUBLICATION pub_{seed} FOR TABLE {tabs};")
    host, port = P.rsplit(':', 1)
    SK.run(f"CREATE SUBSCRIPTION sub_{seed} CONNECTION 'host={host} port={port} user=postgres dbname={db}' PUBLICATION pub_{seed} WITH (slot_name = 'slot_{seed}');")
    # a second subscriber with pg_sift and the scope index of its own: the apply worker writes the facts and the pairs to it
    LP.run(shape.base_ddl() + shape.sift_ddl(opts))
    LP.run(f"CREATE SUBSCRIPTION subl_{seed} CONNECTION 'host={host} port={port} user=postgres dbname={db}' PUBLICATION pub_{seed} WITH (slot_name = 'slotl_{seed}');")
    A.run(shape.sift_ddl(opts) + "VACUUM (FREEZE, ANALYZE) " + tabs + ";")
    A.run(("REINDEX INDEX facts_col; " if not walk or rng.random() < 0.5 else '') + "ANALYZE;")
    # the stamps of a scope index are taken by the build after the journals exist
    wait_stock(A, SK, LP)
    lsn = A.val("SELECT pg_current_wal_lsn()")
    wait_standby(A, Pg('standby-admin', S, 'postgres', log, sift=False), lsn)
    SP = Pg('standby', S, db, log, setup)
    gate = Gate(a)
    state = {'next': rows + 1}
    W = Pg('writer', P, db, log, setup)
    M2 = Pg('maint', P, db, log, setup)
    ntenants = shape.T
    r = random.Random(seed * 7919 + 1)

    def quiesce(step, label):
        """The battery on every node, compared with the plain plan on the primary and with the stock node."""
        qs = shape.queries(r, ntenants)
        # the three arms in one snapshot: switch the arm with SET LOCAL between the groups of statements
        text = "BEGIN ISOLATION LEVEL REPEATABLE READ;"
        for arm in ('scope', 'planned', 'plain'):
            text += "\n" + ARMS[arm]
            for name, q in qs:
                text += f"\nSELECT '{arm}.{name}', ({digest(q)});"
        text += "\nCOMMIT;"
        out_ = A.run(text)
        res = {m.group(1): m.group(2) for m in re.finditer(r'^([a-z]+\.\w+)\|(\S+)$', out_, re.M)}
        if len(res) != 3 * len(qs):
            raise Fail(f'primary battery gave {len(res)} answers: {out_[-300:]}')
        by = {arm: {n: res[f'{arm}.{n}'] for n, _ in qs} for arm in ('scope', 'planned', 'plain')}
        gate.compare(seed, step, f'{label} scope vs plain', qs, by['scope'], by['plain'], A, 'scope', A, 'plain')
        gate.compare(seed, step, f'{label} planned vs plain', qs, by['planned'], by['plain'], A, 'planned', A, 'plain')
        wait_stock(A, SK, LP)
        lsn = A.val("SELECT pg_current_wal_lsn()")
        stock = answers(SK, 'stock', qs)
        gate.compare(seed, step, f'{label} plain vs the stock node', qs, by['plain'], stock, A, 'plain', SK, 'stock')
        gate.compare(seed, step, f'{label} scope vs the stock node', qs, by['scope'], stock, A, 'scope', SK, 'stock')
        for arm in ('scope', 'planned', 'plain'):
            la = answers(LP, arm, qs)
            gate.compare(seed, step, f'{label} logical subscriber with its own scope index, {arm} vs the stock node', qs, la, stock, LP, arm, SK, 'stock')
        if step == 0 or step % a.toggle_every == 0:
            toggle(step, qs, stock, A)
            toggle(step, qs, stock, LP, 'logical subscriber')
        wait_standby(A, SP, lsn)
        for arm in ('scope', 'planned', 'plain'):
            sa = answers(SP, arm, qs)
            gate.compare(seed, step, f'{label} standby {arm} vs the stock node', qs, sa, stock, SP, arm, SK, 'stock')
        # does the scope path engage (statistics only)
        for name, q in qs:
            plan = A.run(ARMS['scope'].replace('LOCAL ', '') + f" EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS OFF) {q}")
            m = re.search(r'Scope Fallback: (.*)', plan)
            how = 'units' if 'Scope Units:' in plan and not m else (m.group(1).strip() if m else ('no scope node' if 'Scope Index' not in plan else 'scope node, no units'))
            STATS['scope_reads'] += 1
            STATS['engaged'] += how == 'units'
            WHY[f'{shape.name}/{name}: {how}'] = WHY.get(f'{shape.name}/{name}: {how}', 0) + 1
        A.run("RESET sift.enable_agg; RESET sift.agg_force; RESET sift.enable_col_scan; RESET enable_indexscan; RESET enable_bitmapscan; RESET enable_indexonlyscan;")
        return qs

    def verify(step):
        for who, pg in (('primary', A), ('standby', SP), ('logical subscriber', LP)):
            o = pg.run("SELECT scope_key, problem, detail FROM sift.col_scope_verify('facts_col', 6);")
            STATS['verify'] += 1
            STATS['unverified'] += o.count('unverified')
            if 'differs' in o:
                gate.fail(seed, step, f'sift.col_scope_verify on the {who} reports a differing key', 'col_scope_verify', '', o)

    def toggle(step, qs, ref, pg=None, who='primary'):
        """The same requests in ONE session with sift.agg_scope off then on, on then off, and with the planner's own choice and the forced one: every answer is the plain plan's."""
        pg = pg or A
        for force in ('on', 'off'):
            for order in (('off', 'on'), ('on', 'off'), ('off', 'on', 'off')):
                for mode in order:
                    pg.run(f"SET sift.agg_scope = {mode}; SET sift.enable_agg = on; SET sift.agg_force = {force};")
                    got = answers(pg, 'session', qs)
                    gate.compare(seed, step, f'{who}: one session, sift.agg_scope {"->".join(order)} (now {mode}), agg_force {force}', qs, got, ref, pg, 'session', pg, 'plain')
        pg.run("RESET sift.agg_scope; RESET sift.agg_force; RESET sift.enable_agg;")

    def own_command(step):
        """Reads inside the transaction that edits: every read is the plain plan's, under the command ids of the transaction."""
        STATS['own'] += 1
        qs = shape.queries(r, ntenants)[:5]
        W.run("BEGIN;")
        for i in range(r.randint(2, 4)):
            edit(W, (shape.pair_edit(r) if r.random() < 0.6 else fact_edit(r, shape.M, state)))
            sub = r.random() < 0.3
            if sub:
                edit(W, "SAVEPOINT own;" + shape.pair_edit(r))
            s_ = answers(W, 'scope', qs, snapshot=False)
            p_ = answers(W, 'plain', qs, snapshot=False)
            gate.compare(seed, step, f'inside the editing transaction (edit {i})', qs, s_, p_, W, 'scope', W, 'plain', True)
            if sub:
                if r.random() < 0.5:
                    W.run("ROLLBACK TO own;")
                    s_ = answers(W, 'scope', qs, snapshot=False)
                    p_ = answers(W, 'plain', qs, snapshot=False)
                    gate.compare(seed, step, 'after ROLLBACK TO SAVEPOINT', qs, s_, p_, W, 'scope', W, 'plain', True)
                else:
                    W.run("RELEASE own;")
        W.run(r.choice(["COMMIT;", "ROLLBACK;"]))
        W.run("RESET sift.enable_agg; RESET sift.agg_force; RESET sift.enable_col_scan; RESET enable_indexscan; RESET enable_bitmapscan; RESET enable_indexonlyscan;")

    def snapshot_window(step):
        """An old snapshot (REPEATABLE READ or SERIALIZABLE) across committed edits, on the primary and on the standby."""
        STATS['snapshots'] += 1
        qs = shape.queries(r, ntenants)
        level = r.choice(['REPEATABLE READ', 'SERIALIZABLE READ ONLY'])
        R1 = Pg('snap', P, db, log, setup)
        R1.run(f"BEGIN ISOLATION LEVEL {level}; SELECT count(*) FROM facts;")
        first = answers(R1, 'plain', qs, snapshot=False)
        for _ in range(r.randint(1, 2)):
            edit(W, shape.pair_edit(r)); edit(W, fact_edit(r, shape.M, state))
        if r.random() < 0.5:
            M2.run(r.choice(["VACUUM facts;", "VACUUM pairs;" if shape.name != 'union' else "VACUUM grants;", "SELECT sift.col_promote('facts_col');", "SELECT sift.col_merge('facts_col', true);"]))
        for arm in ('scope', 'planned', 'plain'):
            got = answers(R1, arm, qs, snapshot=False)
            gate.compare(seed, step, f'old {level} snapshot, {arm} vs the first read', qs, got, first, R1, arm, R1, 'plain', True)
        R1.run("COMMIT;")
        R1.close()

    def open_writer(step):
        """A writer's transaction stays open across maintenance and reads; its rows are invisible to the others."""
        W2 = Pg('open-writer', P, db, log, setup)
        before = shape.queries(r, ntenants)
        base = answers(A, 'plain', before)
        edit(W2, "BEGIN;" + fact_edit(r, shape.M, state) + shape.pair_edit(r))
        M2.run(r.choice(["VACUUM facts;", "SELECT sift.col_promote('facts_col');", "SELECT sift.col_merge('facts_col', true);", "VACUUM;"]))
        for arm in ('scope', 'plain'):
            got = answers(A, arm, before)
            gate.compare(seed, step, f'a writer holds a transaction open, {arm} vs the state before it', before, got, base, A, arm, A, 'plain')
        W2.run(r.choice(["COMMIT;", "ROLLBACK;"]))
        W2.close()

    def concurrent_window(step, secs):
        """pgbench writers; REPEATABLE READ and SERIALIZABLE readers on the primary and the standby compare inside their snapshots; a CREATE INDEX CONCURRENTLY runs in it."""
        STATS['windows'] += 1
        d = os.path.join(out, f'bench{seed}_{step}')
        os.makedirs(d, exist_ok=True)
        M, T, G = shape.M, shape.T, shape.G
        open(f'{d}/f.sql', 'w').write(
            f"\\set a random(1, {rows * 2})\n\\set m random(1, {M + 20})\n\\set n random(1, 6)\n"
            "BEGIN;\nUPDATE facts SET amount = amount + 1 WHERE id BETWEEN :a AND :a + 30;\n"
            "UPDATE facts SET dim_a_id = :m WHERE id BETWEEN :a + 40 AND :a + 50;\nUPDATE facts SET pad = 'b' WHERE id BETWEEN :a + 60 AND :a + 80;\n"
            f"INSERT INTO facts SELECT {rows * 4} + :a * 10 + i, :m, now(), i % 7, i, 'c' FROM generate_series(1, :n) i ON CONFLICT (id) DO NOTHING;\n"
            "DELETE FROM facts WHERE id BETWEEN :a + 100 AND :a + 104;\nCOMMIT;\n")
        if shape.name == 'plain':
            ps = (f"\\set m random(1, {M})\n\\set k random(1, {T})\n\\set k2 random(1, {T})\nBEGIN;\nINSERT INTO pairs VALUES (:m, :k) ON CONFLICT DO NOTHING;\n"
                  f"DELETE FROM pairs WHERE dim_a_id = :m + 1 AND tenant_id = :k2;\nWITH d AS (DELETE FROM pairs WHERE dim_a_id = :m + 2 AND tenant_id = :k RETURNING dim_a_id) "
                  "INSERT INTO pairs SELECT dim_a_id, :k2 FROM d ON CONFLICT DO NOTHING;\nCOMMIT;\n")
        elif shape.name == 'soft':
            ps = (f"\\set m random(1, {M})\n\\set k random(1, {T})\nBEGIN;\nINSERT INTO pairs VALUES (:m, :k) ON CONFLICT (dim_a_id, tenant_id) DO UPDATE SET deleted_at = NULL;\n"
                  "UPDATE pairs SET deleted_at = now() WHERE dim_a_id = :m + 1 AND deleted_at IS NULL;\nUPDATE pairs SET deleted_at = NULL WHERE dim_a_id = :m + 3 AND tenant_id = :k;\nCOMMIT;\n")
        else:
            ps = (f"\\set m random(1, {M})\n\\set k random(1, {T})\n\\set g random(1, {G})\nBEGIN;\nINSERT INTO group_members (group_id, dim_a_id) VALUES (:g, :m) ON CONFLICT DO NOTHING;\n"
                  "UPDATE group_members SET deleted_at = now() WHERE dim_a_id = :m + 1 AND deleted_at IS NULL;\nINSERT INTO grants (tenant_id, group_id) VALUES (:k, :g) ON CONFLICT DO NOTHING;\n"
                  "DELETE FROM grants WHERE id = :m + :k;\nCOMMIT;\n")
        open(f'{d}/p.sql', 'w').write(ps)
        host, port = P.rsplit(':', 1)
        bench = subprocess.Popen(f"pgbench -n -h {host} -p {port} -U postgres -c 3 -j 2 -T {secs} -f {d}/f.sql@6 -f {d}/p.sql@4 {db}", shell=True,
                                 stdout=open(f'{d}/pgbench.log', 'w'), stderr=subprocess.STDOUT)
        qs = shape.queries(r, ntenants)
        t_end = time.time() + secs - 1
        cic = r.random() < 0.7
        did = False
        n = 0
        while time.time() < t_end and bench.poll() is None:
            for who, node, pgdb in (('primary', P, None), ('standby', S, None)):
                lvl = r.choice(['REPEATABLE READ', 'SERIALIZABLE READ ONLY']) if who == 'primary' else 'REPEATABLE READ'
                R1 = Pg('window-' + who, node, db, log, setup if who == 'primary' else setup)
                try:
                    R1.run(f"BEGIN ISOLATION LEVEL {lvl}; SELECT count(*) FROM facts;")
                    first = answers(R1, 'plain', qs, snapshot=False)
                    for arm in ('scope', 'planned'):
                        got = answers(R1, arm, qs, snapshot=False)
                        gate.compare(seed, step, f'window {who} {lvl} {arm} vs plain', qs, got, first, R1, arm, R1, 'plain', True)
                    again = answers(R1, 'plain', qs, snapshot=False)
                    gate.compare(seed, step, f'window {who} {lvl} plain twice', qs, again, first, R1, 'plain', R1, 'plain', True)
                    R1.run("COMMIT;")
                except Fail as e:
                    if re.search(r'conflict with recovery|could not serialize|terminating connection due to conflict', str(e)):
                        R1.close()
                        continue
                    raise
                R1.close()
            n += 1
            if cic and not did and n >= 2:
                did = True
                M2.run(shape.scope_index('facts_col2', opts, True), allow=r'.')
                M2.run("SELECT sift.col_promote('facts_col');" if r.random() < 0.5 else "VACUUM facts;")
        bench.wait()
        if bench.returncode not in (0, None):
            lg = open(f'{d}/pgbench.log').read()
            if 'ERROR' in lg and 'deadlock' not in lg and 'serialize' not in lg:
                raise Fail(f'pgbench: {lg[-400:]}')
        return did

    steps = a.steps
    quiesce(0, 'after the build')
    for step in range(1, steps + 1):
        for _ in range(r.randint(1, 3)):
            edit(W, r.choice([lambda: shape.pair_edit(r), lambda: fact_edit(r, shape.M, state), lambda: fact_edit(r, shape.M, state)])())
        if r.random() < 0.7:
            W.run(r.choice(["VACUUM facts;", "VACUUM (ANALYZE) " + ('pairs' if shape.name != 'union' else 'grants, group_members') + ";", "VACUUM (FREEZE) facts;"]))
        if r.random() < 0.4:
            M2.run(r.choice(["SELECT sift.col_promote('facts_col');", "SELECT sift.col_merge('facts_col', false);", "SELECT sift.col_merge('facts_col', true);"]))
        if r.random() < 0.15:
            M2.run(r.choice(["REINDEX INDEX facts_col;", "REINDEX INDEX CONCURRENTLY facts_col;"]), allow=r'.')
        quiesce(step, f'step {step}')
        if r.random() < 0.6:
            own_command(step)
        if r.random() < 0.5:
            snapshot_window(step)
        if r.random() < 0.4:
            open_writer(step)
        if step % 4 == 0:
            verify(step)
    # windows of concurrent writers, a CREATE INDEX CONCURRENTLY in one of them
    for w in range(a.windows):
        cic = concurrent_window(steps + 1 + w, a.window_secs)
        quiesce(steps + 1 + w, f'after window {w}')
        has2 = A.val("SELECT count(*) FROM pg_class WHERE relname = 'facts_col2'") == '1'
        STATS['cic'] = STATS.get('cic', 0) + bool(cic)
        if has2:
            verify(steps + 1 + w)
            A.run("DROP INDEX facts_col2;")
    verify(steps + 10)
    # dump and restore: the restored database reads as the stock node does
    dump = f'{out}/seed{seed}.dump'
    host, port = P.rsplit(':', 1)
    shell(f"pg_dump -Fc -h {host} -p {port} -U postgres -f {dump} {db}")
    rh, rp = R.rsplit(':', 1)
    shell(f"pg_restore -h {rh} -p {rp} -U postgres -d {db} --no-owner {dump}")
    RP = Pg('restore', R, db, log, setup)
    RP.run("ANALYZE; VACUUM facts;")
    wait_stock(A, SK, LP)
    qs = shape.queries(r, ntenants)
    stock = answers(SK, 'stock', qs)
    for arm in ('scope', 'planned', 'plain'):
        gate.compare(seed, 'restore', f'restored {arm} vs the stock node', qs, answers(RP, arm, qs), stock, RP, arm, SK, 'stock')
    o = RP.run("SELECT scope_key, problem, detail FROM sift.col_scope_verify('facts_col', 6);")
    STATS['verify'] += 1
    if 'differs' in o:
        gate.fail(seed, 'restore', 'sift.col_scope_verify on the restored database reports a differing key', 'col_scope_verify', '', o)
    LP.run(f"ALTER SUBSCRIPTION subl_{seed} DISABLE; ALTER SUBSCRIPTION subl_{seed} SET (slot_name = NONE); DROP SUBSCRIPTION subl_{seed};")
    drop_slot(A, f'slotl_{seed}')
    SK.run(f"ALTER SUBSCRIPTION sub_{seed} DISABLE; ALTER SUBSCRIPTION sub_{seed} SET (slot_name = NONE); DROP SUBSCRIPTION sub_{seed};")
    drop_slot(A, f'slot_{seed}')
    for p in (A, SK, SP, LP, W, M2, RP, root):
        p.close()
    os.remove(dump)
    shutil_rm(f'{out}/bench{seed}_*')


def shutil_rm(pat):
    shell(f'rm -rf {pat}', check=False)


def parse_seeds(s):
    if '-' in s:
        lo, hi = s.split('-')
        return list(range(int(lo), int(hi) + 1))
    return [int(x) for x in s.split(',')]


def main():
    p = argparse.ArgumentParser()
    for k in ('primary', 'standby', 'stock', 'restore', 'out'):
        p.add_argument('--' + k, required=True)
    p.add_argument('--seeds', default='1-3')
    p.add_argument('--rows', type=int, default=60000)
    p.add_argument('--steps', type=int, default=8)
    p.add_argument('--windows', type=int, default=1)
    p.add_argument('--toggle-every', dest='toggle_every', type=int, default=1)
    p.add_argument('--window-secs', dest='window_secs', type=int, default=8)
    a = p.parse_args()
    os.makedirs(a.out, exist_ok=True)
    t0 = time.time()
    try:
        for seed in parse_seeds(a.seeds):
            run_seed(a, seed)
    except Fail as e:
        for p in Pg.ALL:
            p.close()
        print(f'FAIL {e}', flush=True)
        print('stats', STATS, flush=True)
        print('scope reads', sorted(WHY.items()), flush=True)
        sys.exit(1)
    print(f'ok: seeds {a.seeds} in {time.time() - t0:.0f} s; stats {STATS}', flush=True)
    print('scope reads', sorted(WHY.items()), flush=True)


if __name__ == '__main__':
    main()
