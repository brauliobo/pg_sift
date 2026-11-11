#!/usr/bin/env python3
"""Differential fuzz of scope copies (sift.scope_create): the copy of a fact table kept exact by triggers against the join of the facts with the scope pairs.

  test/fuzz/scope_fuzz.py [--seeds 1-12] [--steps 40] [--out DIR] [--stop-after N] [--budget SEC] [--no-shrink] [--keep] [--show]
  test/fuzz/scope_fuzz.py --repro DIR/scope_fuzz_N.sql | --selftest

Needs the scripts/pgtest.sh cluster (SIFT_TMP, SIFT_PORT). A seed gives facts(id, dim_a_id, created_at, kind, amount, note) and scope pairs (dim_a_id, scope_id), with a skewed
multiplicity (most ids in one key, 10% in two or more up to six, some in none, some keys empty; every third seed has one key for each id) and two copies of the facts (built with max_copies, as a seed's multiplicity can pass the default 1.8), a
partitioned one and a plain one, and a script of steps: one statement (insert, update of an amount, of a note, of the foreign key or of the primary key, delete, upsert, MERGE,
COPY, TRUNCATE of the facts or of the pairs, pairs inserted, moved between keys, moved to another id, deleted), a data-modifying CTE that changes facts and pairs in one statement,
a transaction (COMMIT, ROLLBACK, SAVEPOINT with ROLLBACK TO and RELEASE, a check in the middle), VACUUM and the maintenance of the index. After every step (and in a fresh backend after every
fourth) the copies are checked: sift.scope_verify returns no row, the rows equal the join's, and for every key from 0 to 9 (empty ones too) the aggregates by kind and by note, the
newest rows and the rows themselves read by the sift plan (the aggregate scan forced) equal the join read by the plain plan; several keys at once through DISTINCT id and GROUP BY the key
equal the join's. A statement that raises anything but a duplicate key of the facts or of the pairs ends the seed. The first step whose check differs ends the seed and the script is cut
down (steps, then the statements of a transaction) to a reproducer, DIR/scope_fuzz_N.sql, which is printed.
"""
import argparse
import os
import random
import re
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from txn_fuzz.session import Hang, Session, create_db, drop_db, errors, psql  # noqa: E402

IDS, ABSENT, KEYS = 40, 45, 8       # ids 1..40 may have pairs, 41..45 have facts and no pair, 46..48 have pairs and no fact; keys 1..8 (8 is empty at first)
ALLOWED = re.compile(r'duplicate key value violates unique constraint "(facts|pairs)_pkey"')
COPIES = ('cflat', 'cpart')
FEATURES = ['f_ins', 'f_ins_many', 'f_upsert', 'f_upsert_skip', 'f_amount', 'f_note', 'f_fk', 'f_pk', 'f_cols', 'f_del', 'f_del_member', 'f_copy', 'f_merge', 'f_truncate',
            'p_ins', 'p_ins_from', 'p_move', 'p_member', 'p_noop', 'p_del', 'p_del_key', 'p_truncate', 'c_pair_fact', 'c_pairdel_fact', 'c_fact_pairdel', 'c_factdel_pair',
            'c_move_fk', 'txn_commit', 'txn_rollback', 'txn_savepoint', 'vacuum', 'promote', 'merge']
ONE_KEY = {'f_ins', 'f_ins_many', 'f_upsert', 'f_upsert_skip', 'f_amount', 'f_note', 'f_fk', 'f_pk', 'f_cols', 'f_del', 'f_del_member', 'f_copy', 'f_merge', 'p_move', 'p_noop',
           'c_move_fk', 'txn_commit', 'txn_rollback', 'txn_savepoint', 'vacuum', 'promote', 'merge'}
# the weight of a kind (10 otherwise): more inserts than deletes keep the tables from running empty
WEIGHT = {'f_truncate': 1, 'p_truncate': 1, 'vacuum': 2, 'promote': 2, 'merge': 2, 'f_ins': 16, 'f_ins_many': 12, 'p_ins': 16, 'p_ins_from': 12,
          'f_del': 5, 'f_del_member': 4, 'p_del': 5, 'p_del_key': 3}


def helpers():
    """The functions of the check: a query run under the sift plan and under the plain one, and the shapes of the reads"""
    return """
CREATE FUNCTION sf_md5(q text, ordered boolean) RETURNS text LANGUAGE plpgsql AS $f$
DECLARE r text;
BEGIN
	EXECUTE format('SELECT coalesce(md5(string_agg(t::text, ''|'' %s)), ''-'') FROM (%s) t', CASE WHEN ordered THEN '' ELSE 'ORDER BY t::text' END, q) INTO r;
	RETURN r;
END $f$;
CREATE FUNCTION sf_sift(q text, ordered boolean) RETURNS text LANGUAGE sql SET sift.enable_agg = on SET sift.agg_force = on SET sift.enable_col_scan = on
	SET max_parallel_workers_per_gather = 0 RETURN sf_md5(q, ordered);
CREATE FUNCTION sf_plain(q text, ordered boolean) RETURNS text LANGUAGE sql SET sift.enable_agg = off SET sift.enable_col_scan = off SET max_parallel_workers_per_gather = 0
	SET enable_indexscan = off SET enable_bitmapscan = off RETURN sf_md5(q, ordered);
CREATE FUNCTION sf_shape(w text, src text, filter text) RETURNS text LANGUAGE sql IMMUTABLE RETURN CASE w
	WHEN 'kind'  THEN format('SELECT kind, count(*), sum(amount), min(created_at), max(id) FROM %s WHERE %s GROUP BY kind', src, filter)
	WHEN 'note'  THEN format('SELECT note, count(*), sum(amount) FROM %s WHERE %s GROUP BY note', src, filter)
	WHEN 'top'   THEN format('SELECT id, created_at FROM %s WHERE %s ORDER BY created_at DESC, id DESC LIMIT 10', src, filter)
	WHEN 'rows'  THEN format('SELECT id, dim_a_id, created_at, kind, amount, note FROM %s WHERE %s', src, filter)
	WHEN 'ids'   THEN format('SELECT DISTINCT id FROM %s WHERE %s', src, filter)
	WHEN 'count' THEN format('SELECT count(DISTINCT id), count(*), sum(amount) FROM %s WHERE %s', src, filter) END;
CREATE FUNCTION sf_check(copy text) RETURNS SETOF text LANGUAGE plpgsql AS $f$
DECLARE
	k int; w text; j text := 'facts f JOIN pairs p USING (dim_a_id)'; a text; b text; ks int[]; kt text; pred text; i int;
BEGIN
	RETURN QUERY SELECT 'verify ' || copy || ': ' || problem || ' key ' || scope_key || ' ' || row_key FROM (SELECT * FROM sift.scope_verify(copy) LIMIT 3) v;
	EXECUTE format('SELECT (SELECT count(*) FROM %s), (SELECT count(*) FROM %s)', copy, j) INTO a, b;
	IF a <> b THEN RETURN NEXT format('rows %s: the copy has %s, the join %s', copy, a, b); END IF;
	FOR k IN 0..%(max)s LOOP
		FOREACH w IN ARRAY ARRAY['kind', 'note', 'top', 'rows', 'ids', 'count'] LOOP
			IF sf_sift(sf_shape(w, copy, 'scope_id = ' || k), w = 'top') IS DISTINCT FROM sf_plain(sf_shape(w, j, 'scope_id = ' || k), w = 'top') THEN
				RETURN NEXT format('answer %s: %s of key %s differs from the join', copy, w, k);
			END IF;
		END LOOP;
		-- several keys in one read: a fact is once in each key, so the rows are DISTINCT on the primary key
		IF sf_sift(sf_shape('ids', copy, format('scope_id IN (%s, %s)', k, k + 3)), false) IS DISTINCT FROM sf_plain(sf_shape('ids', j, format('scope_id IN (%s, %s)', k, k + 3)), false) THEN
			RETURN NEXT format('answer %s: DISTINCT id of keys %s and %s differs from the join', copy, k, k + 3);
		END IF;
		-- the exact read of 2 and 3 keys: a fact counts under the first requested key that holds its id, which is what the path through the id counts
		FOREACH kt IN ARRAY ARRAY[format('{%s,%s}', k, k + 3), format('{%s,%s,%s}', k, k + 1, k + 3), format('{%s,%s,%s}', k + 3, k + 1, k)] LOOP
			ks := kt::int[];
			pred :=format('c.scope_id = %s', ks[1]);
			FOR i IN 2..array_length(ks, 1) LOOP
				pred := pred || format(' OR (c.scope_id = %s AND NOT EXISTS (SELECT 1 FROM pairs x WHERE x.dim_a_id = c.dim_a_id AND x.scope_id = ANY (%L)))', ks[i], ks[1:i - 1]);
			END LOOP;
			IF sf_sift(format('SELECT kind, count(*), sum(amount), count(DISTINCT id) FROM %s c WHERE %s GROUP BY kind', copy, pred), false) IS DISTINCT FROM
			   sf_plain(format('SELECT kind, count(*), sum(amount), count(DISTINCT id) FROM facts WHERE dim_a_id IN (SELECT dim_a_id FROM pairs WHERE scope_id = ANY (%L)) GROUP BY kind', ks), false) THEN
				RETURN NEXT format('answer %s: the exact read of keys %s differs from the read through the id', copy, ks);
			END IF;
		END LOOP;
	END LOOP;
	-- GROUP BY the key counts a fact in each of its keys
	IF sf_sift(format('SELECT scope_id, count(*), sum(amount) FROM %s GROUP BY scope_id', copy), false) IS DISTINCT FROM
	   sf_plain(format('SELECT scope_id, count(*), sum(amount) FROM %s GROUP BY scope_id', j), false) THEN
		RETURN NEXT format('answer %s: GROUP BY the key differs from the join', copy);
	END IF;
END $f$;
""".replace('%(max)s', str(KEYS + 1))


def skewed_pairs(rng, one_key):
    """-> the (id, key) pairs: ids in 0, 1 and 2..6 keys, most in one (every id in one key when one_key)"""
    pairs = []
    for m in range(1, IDS + 1):
        if one_key:
            n = 1
        elif m in (1, 2, 3, 4, 5, 6):
            n = {1: 6, 2: 4, 3: 2, 4: 0, 5: 3, 6: 5}[m]
        else:
            r = rng.random()
            n = 0 if r < 0.08 else 1 if r < 0.88 else rng.choices([2, 3, 4, 5, 6], [50, 25, 12, 8, 5])[0]
        pairs += [(m, k) for k in rng.sample(range(1, KEYS), n)]
    return pairs + [(m, rng.randint(1, KEYS - 1)) for m in range(ABSENT + 1, ABSENT + 4)]


def setup_sql(seed, one_key):
    rng = random.Random(seed * 7919)
    pairs = ', '.join(f'({m}, {k})' for m, k in skewed_pairs(rng, one_key))
    n = 300 + 100 * (seed % 4)
    return f"""SET client_min_messages = warning;
CREATE EXTENSION IF NOT EXISTS pg_sift;
CREATE TABLE facts (id int PRIMARY KEY, dim_a_id int NOT NULL, created_at timestamptz NOT NULL, kind smallint NOT NULL, amount numeric(8,2) NOT NULL, note text)
	WITH (autovacuum_enabled = false);
CREATE INDEX ON facts (dim_a_id);
CREATE TABLE pairs (dim_a_id int NOT NULL, scope_id int NOT NULL, PRIMARY KEY (scope_id, dim_a_id)) WITH (autovacuum_enabled = false);
INSERT INTO pairs VALUES {pairs};
INSERT INTO facts SELECT i, 1 + (i * 13) % {ABSENT}, timestamptz '2026-01-01' + i * interval '1 hour', i % 5, i % 97, 'n' || i % 7 FROM generate_series(1, {n}) i;
SELECT sift.scope_create('cflat', 'facts', 'dim_a_id', 'pairs', 'scope_id', max_copies => {KEYS});
SELECT sift.scope_create('cpart', 'facts', 'dim_a_id', 'pairs', 'scope_id', partition => true, max_copies => {KEYS});
VACUUM (ANALYZE) cflat;
{helpers()}"""


class Gen:
    """The statements of a script: the literals come from the seeded generator, so a script is the same text on every run"""

    def __init__(self, seed, one_key):
        self.rng, self.one_key, self.next, self.shift, self.n = random.Random(seed), one_key, 300 + 100 * (seed % 4), 0, 0
        self.kinds = [f for f in FEATURES if not one_key or f in ONE_KEY]

    def member(self):
        return self.rng.randint(1, ABSENT + 3)

    def key(self):
        return self.rng.randint(1, KEYS)

    def mr(self):
        m = self.rng.choice([2, 3, 5, 7, 11])
        return m, self.rng.randrange(m)

    def row(self, id=None, member=None):
        if id is None:
            self.next += 1
            id = self.next
        r = self.rng
        return (id, member or self.member(), f'timestamptz \'2026-02-01\' + {r.randint(0, 5000)} * interval \'1 minute\'', r.randint(0, 4), r.randint(0, 99), f"'g{r.randint(0, 6)}'")

    def values(self, n, ids=None):
        return ', '.join('(%d, %d, %s, %d, %d, %s)' % self.row(None if ids is None else ids[i]) for i in range(n))

    def old_ids(self, n):
        return self.rng.sample(range(1, self.next + 1), min(n, self.next))

    def stmt(self, kind):
        r, (m, rem) = self.rng, self.mr()
        a, b = self.key(), self.key()
        d = r.randint(1, 5)
        if kind == 'f_ins':
            return f'INSERT INTO facts VALUES {self.values(r.randint(1, 6))}'
        if kind == 'f_ins_many':
            self.next += 60
            return (f"INSERT INTO facts SELECT {self.next - 60} + i, 1 + (i * {r.randint(1, 40)}) % {ABSENT + 3}, timestamptz '2026-03-01' + i * interval '1 minute', i % 5, i % 90, 'm' || i % 4 "
                    f'FROM generate_series(1, 60) i')
        if kind == 'f_upsert':
            ids = self.old_ids(3) + [self.next + 1]
            self.next += 1
            return f'INSERT INTO facts VALUES {self.values(4, ids)} ON CONFLICT (id) DO UPDATE SET amount = facts.amount + {d}, dim_a_id = EXCLUDED.dim_a_id, note = EXCLUDED.note'
        if kind == 'f_upsert_skip':
            self.next += 2
            return f'INSERT INTO facts VALUES {self.values(4, self.old_ids(2) + [self.next - 1, self.next])} ON CONFLICT (id) DO NOTHING'
        if kind == 'f_amount':
            return f'UPDATE facts SET amount = amount + {d} WHERE id % {m} = {rem}'
        if kind == 'f_note':
            return f"UPDATE facts SET note = 'u{d}' WHERE id % {m} = {rem}"
        if kind == 'f_fk':
            return f'UPDATE facts SET dim_a_id = 1 + (dim_a_id + {r.randint(1, 20)}) % {ABSENT + 3} WHERE id % {m} = {rem}'
        if kind == 'f_pk':
            self.shift += 1000000
            return f'UPDATE facts SET id = id + {self.shift} WHERE id % {m} = {rem} AND id < 1000000'
        if kind == 'f_cols':
            return f"UPDATE facts SET kind = (kind + 1) % 5, amount = amount * 2 % 100, note = note || 'z', created_at = created_at + interval '1 day' WHERE id % {m} = {rem}"
        if kind == 'f_del':
            return f'DELETE FROM facts WHERE id % {m} = {rem}'
        if kind == 'f_del_member':
            return f'DELETE FROM facts WHERE dim_a_id = {r.randint(1, 6)}'
        if kind == 'f_copy':
            rows = '\n'.join('%d\t%d\t2026-04-01 00:00:%02d+00\t%d\t%d\t%s' % (i, mem, k, kd, am, 'c') for k, (i, mem, _, kd, am, _) in
                             enumerate(self.row() for _ in range(r.randint(2, 12))))
            return f'COPY facts (id, dim_a_id, created_at, kind, amount, note) FROM STDIN;\n{rows}\n\\.'
        if kind == 'f_merge':
            ids = self.old_ids(4) + [self.next + 1]
            self.next += 1
            vals = ', '.join('(%d, %d, %d)' % (i, self.member(), r.randint(0, 99)) for i in ids)
            return (f'MERGE INTO facts t USING (VALUES {vals}) s(id, m, a) ON t.id = s.id WHEN MATCHED AND s.a < 20 THEN DELETE '
                    f"WHEN MATCHED THEN UPDATE SET dim_a_id = s.m, amount = s.a WHEN NOT MATCHED THEN INSERT VALUES (s.id, s.m, timestamptz '2026-05-01', 1, s.a, 'mg')")
        if kind == 'f_truncate':
            return 'TRUNCATE facts'
        if kind == 'p_ins':
            return f"INSERT INTO pairs VALUES {', '.join('(%d, %d)' % (self.member(), self.key()) for _ in range(r.randint(1, 5)))} ON CONFLICT DO NOTHING"
        if kind == 'p_ins_from':
            return f'INSERT INTO pairs SELECT dim_a_id, {b} FROM pairs WHERE scope_id = {a} ON CONFLICT DO NOTHING'
        if kind == 'p_move':
            mem = self.member() % IDS + 1
            return (f'UPDATE pairs SET scope_id = {b} WHERE dim_a_id = {mem} AND scope_id = {a} '
                    f'AND NOT EXISTS (SELECT 1 FROM pairs q WHERE q.dim_a_id = {mem} AND q.scope_id = {b})')
        if kind == 'p_member':
            to = self.member()
            return (f'UPDATE pairs SET dim_a_id = {to} WHERE dim_a_id = {self.member()} AND scope_id = {a} '
                    f'AND NOT EXISTS (SELECT 1 FROM pairs q WHERE q.dim_a_id = {to} AND q.scope_id = {a})')
        if kind == 'p_noop':
            return f'UPDATE pairs SET dim_a_id = dim_a_id WHERE dim_a_id % {m} = {rem}'
        if kind == 'p_del':
            return f'DELETE FROM pairs WHERE dim_a_id % {m} = {rem} AND scope_id = {a}'
        if kind == 'p_del_key':
            return f'DELETE FROM pairs WHERE scope_id = {a}'
        if kind == 'p_truncate':
            return 'TRUNCATE pairs'
        if kind == 'c_pair_fact':
            mem = self.member()
            rows = ', '.join('(%d, %d, %s, %d, %d, %s)' % self.row(member=mem) for _ in range(3))
            return f'WITH p AS (INSERT INTO pairs VALUES ({mem}, {a}) ON CONFLICT DO NOTHING RETURNING 1) INSERT INTO facts VALUES {rows}'
        if kind == 'c_pairdel_fact':
            return f'WITH d AS (DELETE FROM pairs WHERE scope_id = {a} RETURNING 1) UPDATE facts SET amount = amount + {d} WHERE id % {m} = {rem}'
        if kind == 'c_fact_pairdel':
            return f'WITH f AS (UPDATE facts SET dim_a_id = 1 + dim_a_id % {ABSENT} WHERE id % {m} = {rem} RETURNING 1) DELETE FROM pairs WHERE dim_a_id % {m} = {rem} AND scope_id = {a}'
        if kind == 'c_factdel_pair':
            return (f'WITH f AS (DELETE FROM facts WHERE id % {m} = {rem} RETURNING dim_a_id) '
                    f'INSERT INTO pairs SELECT DISTINCT dim_a_id, {a} FROM f WHERE dim_a_id <= {ABSENT + 3} ON CONFLICT DO NOTHING')
        if kind == 'c_move_fk':
            mem = self.member() % IDS + 1
            return (f'WITH p AS (UPDATE pairs SET scope_id = {b} WHERE dim_a_id = {mem} AND scope_id = {a} AND NOT EXISTS '
                    f'(SELECT 1 FROM pairs q WHERE q.dim_a_id = {mem} AND q.scope_id = {b}) RETURNING 1) UPDATE facts SET dim_a_id = {mem} WHERE id % {m} = {rem}')
        raise ValueError(kind)

    def single(self):
        """One non-transaction kind"""
        return self.stmt(self.rng.choice([k for k in self.kinds if not k.startswith(('txn', 'vacuum', 'promote', 'merge'))]))

    def step(self):
        """-> ('sql', [statements]) outside a transaction block, or ('txn', [statements with BEGIN and the end]), or ('maint', [statement])"""
        weights = [WEIGHT.get(k, 10) for k in self.kinds]
        kind = self.rng.choices(self.kinds, weights)[0]
        if kind == 'vacuum':
            return 'maint', ['VACUUM facts, pairs, cflat, cpart']
        if kind == 'promote':
            return 'maint', ["SELECT sift.col_promote('cflat_col')"]
        if kind == 'merge':
            return 'maint', ["SELECT sift.col_merge('cflat_col', true)"]
        if not kind.startswith('txn'):
            return 'sql', [self.stmt(kind)]
        s = ['BEGIN']
        for _ in range(self.rng.randint(1, 3)):
            s.append(self.single())
        if kind == 'txn_savepoint':
            s.append('SAVEPOINT sp1')
            s += [self.single() for _ in range(self.rng.randint(1, 2))]
            s.append(self.rng.choice(['ROLLBACK TO sp1', 'RELEASE sp1']))
            s += [self.single() for _ in range(self.rng.randint(0, 2))]
        if self.rng.random() < 0.4:
            s.append('CHECK')
        s.append('ROLLBACK' if kind == 'txn_rollback' or (kind == 'txn_savepoint' and self.rng.random() < 0.3) else 'COMMIT')
        return 'txn', s


def script(seed, steps):
    g = Gen(seed, one_key=seed % 4 == 3)
    return [g.step() for _ in range(steps)]


def check_sql():
    return '\n'.join(f"SELECT * FROM sf_check('{c}');" for c in COPIES)


def sift_plan(db):
    """The reads the check compares run by the sift aggregate scan on both copies: a check that read the heap would compare the heap with itself"""
    sets = 'SET sift.enable_agg = on; SET sift.agg_force = on; SET sift.enable_col_scan = on; SET max_parallel_workers_per_gather = 0;'
    for c in COPIES:
        plan = psql(db, sets + f'EXPLAIN (COSTS OFF) {SHAPE_KIND.format(c)}')[0]
        if 'SiftAgg' not in plan:
            return f'the aggregate of {c} is not read by the sift scan: {plan.strip()[:200]}'
    return None


SHAPE_KIND = 'SELECT kind, count(*), sum(amount), min(created_at), max(id) FROM {} WHERE scope_id = 3 GROUP BY kind'


def failure(out):
    """-> a description of what a run's lines say is wrong, or None"""
    bad = [l for l in out if l.startswith(('verify ', 'rows ', 'answer ')) or re.match(r'(ERROR|FATAL|PANIC):', l) and not ALLOWED.search(l) or 'sift.scope_' in l]
    return ' | '.join(bad[:6]) if bad else None


class Run:
    """The sessions of one script on one database"""

    def __init__(self, db):
        self.db, self.s, self.checks, self.errors = db, Session(db), 0, 0

    def close(self):
        self.s.close()

    def exec(self, sql):
        out = self.s.run(sql)
        self.errors += len(errors(out))
        return out

    def check(self, fresh=False):
        self.checks += 1
        if not fresh:
            return failure(self.s.run(check_sql()))
        s = Session(self.db)
        try:
            return failure(s.run(check_sql()))
        finally:
            s.close()

    def step(self, kind, stmts, nth):
        """-> the failure of the step, or None"""
        if kind == 'txn':
            for st in stmts:
                if st == 'CHECK':
                    f = self.check()
                else:
                    out = self.exec(st)
                    f = failure(out)
                    if errors(out) and not f:       # an allowed error aborts the block: the rest of it is not run
                        self.exec('ROLLBACK')
                        break
                if f:
                    self.exec('ROLLBACK')
                    return f
        else:
            for st in stmts:
                f = failure(self.exec(st))
                if f:
                    return f
        return self.check() or (self.check(fresh=True) if nth % 4 == 0 else None)


def run_script(db, steps):
    """-> (index of the failing step, text) or None, and the number of checks and of tolerated errors"""
    r = Run(db)
    try:
        for i, (kind, stmts) in enumerate(steps):
            try:
                f = r.step(kind, stmts, i + 1)
            except Hang as e:
                f = f'HANG: {e}'
            if f:
                return (i, f), r.checks, r.errors
        return None, r.checks, r.errors
    finally:
        r.close()


def ddmin(items, test):
    """The smallest sublist (of the chunks tried) for which test still holds"""
    n = 2
    while len(items) >= 2:
        size = max(len(items) // n, 1)
        cuts = [items[i:i + size] for i in range(0, len(items), size)]
        for i in range(len(cuts)):
            rest = [x for j, ch in enumerate(cuts) if j != i for x in ch]
            if test(rest):
                items, n = rest, max(n - 1, 2)
                break
        else:
            if size == 1:
                break
            n = min(n * 2, len(items))
    return items


def shrink(seed, one_key, steps, at, prefix):
    tpl, db, runs = f'{prefix}_tpl', f'{prefix}_w', [0]
    create_db(tpl)
    psql(tpl, setup_sql(seed, one_key))

    def fails(sub):
        if runs[0] >= 120:
            return False
        runs[0] += 1
        create_db(db, tpl)
        return run_script(db, sub)[0] is not None
    steps = ddmin(steps[:at + 1], fails)
    for i in range(len(steps)):
        kind, stmts = steps[i]
        if kind == 'txn' and len(stmts) > 3:
            inner = ddmin(stmts[1:-1], lambda sub: fails(steps[:i] + [(kind, [stmts[0]] + sub + [stmts[-1]])] + steps[i + 1:]))
            steps[i] = (kind, [stmts[0]] + inner + [stmts[-1]])
    drop_db(tpl, db)
    return steps


def render(seed, one_key, steps, why):
    """-> (the setup, the steps): the reproducer is the two, a psql script that prints the first check that differs"""
    s = ['\\set ON_ERROR_STOP off']
    for kind, stmts in steps:
        s += [check_sql() if st == 'CHECK' else st if st.endswith('\\.') else st + ';' for st in stmts]
        s.append(check_sql())
    return f'-- scope_fuzz seed {seed}: {why}\n' + setup_sql(seed, one_key), '\n'.join(s) + '\n'


def seeds_of(spec):
    out = []
    for part in spec.split(','):
        lo, _, hi = part.partition('-')
        out += range(int(lo), int(hi or lo) + 1)
    return out


def selftest():
    seen = set()
    for seed in range(40):
        steps = script(seed, 30)
        if len(steps) != 30 or not setup_sql(seed, seed % 4 == 3).strip() or any(not s for _, s in steps):
            sys.exit(f'scope_fuzz selftest: seed {seed} made no script')
        for kind, stmts in steps:
            seen.update(st.split()[0] for st in stmts)
            if kind == 'txn' and (stmts[0] != 'BEGIN' or stmts[-1] not in ('COMMIT', 'ROLLBACK')):
                sys.exit(f'scope_fuzz selftest: seed {seed} made a transaction without its ends')
        if seed % 4 == 3 and any(re.match(r'(INSERT INTO pairs|DELETE FROM pairs|TRUNCATE pairs)', st) for _, ss in steps for st in ss):
            sys.exit(f'scope_fuzz selftest: seed {seed} (one key for each id) changes the number of keys of an id')
        if script(seed, 30) != steps:
            sys.exit(f'scope_fuzz selftest: seed {seed} is not the same script twice')
    g = Gen(1, False)
    for k in FEATURES:
        if k.startswith(('txn', 'vacuum', 'promote', 'merge')):
            continue
        if not g.stmt(k):
            sys.exit(f'scope_fuzz selftest: no statement of {k}')
    need = {'INSERT', 'UPDATE', 'DELETE', 'MERGE', 'COPY', 'TRUNCATE', 'WITH', 'BEGIN', 'SAVEPOINT', 'ROLLBACK', 'COMMIT', 'VACUUM', 'CHECK'}
    if need - seen:
        sys.exit(f'scope_fuzz selftest: no statement of {sorted(need - seen)} in 40 seeds')
    pairs = skewed_pairs(random.Random(7919), False)
    count = {m: sum(1 for p in pairs if p[0] == m) for m in range(1, IDS + 1)}
    if not ({0, 1, 6} <= set(count.values()) and all(count[m] <= 6 for m in count)):
        sys.exit('scope_fuzz selftest: the multiplicity of the ids does not span 0, 1 and 6 keys')
    if ddmin(list(range(24)), lambda xs: 3 in xs and 17 in xs) != [3, 17]:
        sys.exit('scope_fuzz selftest: ddmin does not find the two steps that fail')
    print('scope_fuzz selftest: 40 scripts')
    return 0


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--seeds', default='1')
    ap.add_argument('--steps', type=int, default=40, help='steps of a script')
    ap.add_argument('--out', default='/tmp/scope_fuzz')
    ap.add_argument('--db', default='sfz', help='prefix of the databases')
    ap.add_argument('--stop-after', type=int, default=0, help='stop after this many failing seeds (0: all seeds)')
    ap.add_argument('--budget', type=int, default=0, help='stop starting seeds after this many seconds (0: no limit)')
    ap.add_argument('--no-shrink', action='store_true')
    ap.add_argument('--keep', action='store_true', help='keep the databases of the seeds')
    ap.add_argument('--show', action='store_true', help='print every statement')
    ap.add_argument('--repro', default='', help='run a reproducer (the .sql of a failure) in a new database and print what it says')
    ap.add_argument('--selftest', action='store_true', help='no server: the scripts of seeds 0 to 39 are made and the minimiser is exercised (the lint job)')
    a = ap.parse_args()
    if a.selftest:
        return selftest()
    if a.repro:
        create_db(f'{a.db}_repro')
        out, err = psql(f'{a.db}_repro', open(a.repro).read(), check=False)
        print(out + err)
        return 0
    failed, start = 0, time.time()
    for seed in seeds_of(a.seeds):
        if a.budget and time.time() - start > a.budget:
            break
        one_key, db, t0 = seed % 4 == 3, f'{a.db}_{seed}', time.time()
        steps = script(seed, a.steps)
        create_db(db)
        psql(db, setup_sql(seed, one_key))
        if a.show:
            for kind, stmts in steps:
                print('   ', kind, ' ; '.join(stmts)[:300].replace('\n', ' '))
        why = sift_plan(db)
        if why:
            sys.exit(f'scope_fuzz: seed {seed}: {why}')
        fail, checks, errs = run_script(db, steps)
        left = psql(db, 'SELECT (SELECT count(*) FROM facts) || " facts, " || (SELECT count(*) FROM pairs) || " pairs, " || (SELECT count(*) FROM cflat) || " copy rows"'.replace('"', "'"), check=False)[0].strip()
        print(f"seed {seed}: {'FAIL' if fail else 'ok'} {len(steps)} steps, {checks} checks, {errs} tolerated errors, {time.time() - t0:.0f} s "
              f"[{'one key each' if one_key else 'skewed keys'}; at the end {left}]", flush=True)
        if fail:
            failed += 1
            print(f'  step {fail[0] + 1}: {fail[1][:600]}', flush=True)
            if not a.no_shrink:
                steps = shrink(seed, one_key, steps, fail[0], f'{a.db}_s{seed}')
            os.makedirs(a.out, exist_ok=True)
            path = f'{a.out}/scope_fuzz_{seed}.sql'
            head, body = render(seed, one_key, steps, fail[1][:300])
            open(path, 'w').write(head + body)
            print(f'  the script of {len(steps)} steps, after the setup of the seed ({path}):\n' + '\n'.join('    ' + l for l in body.split('\n') if l and not l.startswith('SELECT * FROM sf_check')), flush=True)
        if not a.keep:
            drop_db(db)
        if fail and a.stop_after and failed >= a.stop_after:
            break
    print(f'scope_fuzz: {failed} failing seeds')
    return 1 if failed else 0


if __name__ == '__main__':
    sys.exit(main())
