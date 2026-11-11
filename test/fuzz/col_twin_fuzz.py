#!/usr/bin/env python3
"""Differential fuzz of the order arrays of a sift_col index under churn (test/t/col_twin_fuzz.sh runs it).

  test/fuzz/col_twin_fuzz.py [--seeds 1-24] [--rows 30000] [--steps 10]    needs PGHOST / PGPORT of a cluster with pg_sift, SIFT_FUZZ_OUT, a directory for the logs

A seed fills a table with keys of a chosen shape (many equal keys, a NULL region, scattered or arriving in order), builds two sift_col indexes (order arrays on four columns, and
grouped by an entity) and b-tree twins, and draws steps: batches of INSERTs, DELETEs and UPDATEs of a random, newest or oldest window, upserts, updates that change a key and
updates that do not, an aborted transaction, repeated updates of the same rows, promotion, VACUUM, the fold of the marks of the tombstone log into the liveness runs, merge, REINDEX and ANALYZE. After every step a sample of the queries (top-N both ways, OFFSET small and deep,
ranges with LIMIT, IN lists, group top-N, NULL regions, text keys) must give the same ordered rows through the forced sift plan, the planned one, a forced b-tree plan and a
forced heap plan, and col_verify must pass. A REPEATABLE READ reader opened before the churn must read, through the arrays and through the heap, the rows it first read. The
seed also draws track_changes on or off, sift.col_wbuf_ref, key_cut, the segment size and, in a testing build, sift.test_col_gorder_seek (every chunk seekable).
"""
import argparse, os, random, re, subprocess, sys, time

SOCK, PORT = os.environ['PGHOST'], os.environ['PGPORT']
DB = os.environ.get('PGDATABASE', 'postgres')
OUT = os.environ['SIFT_FUZZ_OUT']
ERR = re.compile(r'^(ERROR|FATAL|PANIC|server closed|connection to server|psql: error)', re.M)

LIB = r"""
CREATE TABLE ct_q (qid int PRIMARY KEY, class text, q text);
CREATE TABLE ct_res (stage text, qid int, class text, gord bool, ok bool, detail text);

CREATE FUNCTION ct_arm(arm text) RETURNS void LANGUAGE plpgsql AS $$
BEGIN
	PERFORM set_config(k, v, true) FROM (VALUES ('sift.enable_agg', 'on'), ('sift.enable_col_scan', 'on'), ('enable_indexscan', 'on'), ('enable_bitmapscan', 'on'),
		('enable_indexonlyscan', 'on'), ('sift.agg_force', 'off'), ('enable_seqscan', 'on'), ('enable_sort', 'on')) s(k, v);
	IF arm = 'sift' THEN
		PERFORM set_config(k, v, true) FROM (VALUES ('enable_indexscan', 'off'), ('enable_bitmapscan', 'off'), ('enable_indexonlyscan', 'off'),
			('sift.agg_force', 'on'), ('enable_seqscan', 'off'), ('enable_sort', 'off')) s(k, v);
	ELSIF arm = 'btree' THEN
		PERFORM set_config(k, v, true) FROM (VALUES ('sift.enable_agg', 'off'), ('sift.enable_col_scan', 'off'), ('enable_seqscan', 'off'), ('enable_bitmapscan', 'off')) s(k, v);
	ELSIF arm = 'heap' THEN
		PERFORM set_config(k, v, true) FROM (VALUES ('sift.enable_agg', 'off'), ('sift.enable_col_scan', 'off'), ('enable_indexscan', 'off'), ('enable_bitmapscan', 'off'),
			('enable_indexonlyscan', 'off')) s(k, v);
	END IF;
END $$;

CREATE FUNCTION ct_md5(q text) RETURNS text LANGUAGE plpgsql AS $$
DECLARE r text;
BEGIN
	EXECUTE 'SELECT coalesce(md5(string_agg(x::text, '','' ORDER BY ord)), ''empty'') || ''/'' || count(*) FROM (SELECT row_number() OVER () AS ord, * FROM (' || q || ') r) x' INTO r;
	RETURN r;
END $$;

-- the queries with qid % every = off through four arms; a row of ct_res per query
CREATE FUNCTION ct_run(stage text, every int, off int) RETURNS void LANGUAGE plpgsql AS $$
DECLARE d record; hs text; hb text; hh text; hd text; pl text; gord bool; ok bool; det text;
BEGIN
	FOR d IN SELECT * FROM ct_q WHERE qid % every = off ORDER BY qid LOOP
		gord := false; det := NULL;
		BEGIN
			PERFORM ct_arm('sift');
			FOR pl IN EXECUTE 'EXPLAIN (COSTS off, TIMING off, SUMMARY off) ' || d.q LOOP gord := gord OR pl LIKE '%Global Order%'; END LOOP;
			hs := ct_md5(d.q);
			PERFORM ct_arm('dflt'); hd := ct_md5(d.q);
			PERFORM ct_arm('btree'); hb := ct_md5(d.q);
			PERFORM ct_arm('heap'); hh := ct_md5(d.q);
			ok := hs = hh AND hb = hh AND hd = hh;
			IF NOT ok THEN det := format('sift=%s planned=%s btree=%s heap=%s', hs, hd, hb, hh); END IF;
		EXCEPTION WHEN OTHERS THEN
			ok := false; det := 'ERROR ' || SQLERRM;
		END;
		INSERT INTO ct_res VALUES (stage, d.qid, d.class, gord, ok, det);
	END LOOP;
	PERFORM ct_arm('dflt');
END $$;

-- the reader: the rows of the first read of a query (heap) must stay the rows of every later read, through the arrays and through the heap
CREATE FUNCTION ct_snap(every int, off int) RETURNS text LANGUAGE plpgsql AS $$
DECLARE d record; hs text; hh text; h0 text; bad text := '';
BEGIN
	CREATE TEMP TABLE IF NOT EXISTS ct_snap0 (qid int PRIMARY KEY, h text);
	FOR d IN SELECT * FROM ct_q WHERE qid % every = off ORDER BY qid LOOP
		PERFORM ct_arm('heap'); hh := ct_md5(d.q);
		PERFORM ct_arm('sift'); hs := ct_md5(d.q);
		INSERT INTO ct_snap0 VALUES (d.qid, hh) ON CONFLICT DO NOTHING;
		SELECT h INTO h0 FROM ct_snap0 WHERE qid = d.qid;
		IF hs <> hh OR hh <> h0 THEN bad := bad || format('q%s sift=%s heap=%s first=%s; ', d.qid, hs, hh, h0); END IF;
	END LOOP;
	PERFORM ct_arm('dflt');
	RETURN bad;
END $$;

CREATE FUNCTION ct_verify(idx text) RETURNS text LANGUAGE plpgsql AS $$
BEGIN
	PERFORM sift.col_verify(idx::regclass, true);
	RETURN 'ok';
EXCEPTION WHEN OTHERS THEN
	RETURN 'ERROR ' || SQLERRM;
END $$;
"""


def sqlstr(s):
    return "'" + s.replace("'", "''") + "'"


def queries(n):
    q = []
    for c in ('ts', 'u', 'k', 's'):
        for d in ('ASC', 'DESC'):
            for lim in (1, 20, 500):
                q.append((f'top_{c}', f'SELECT id, {c} FROM ct ORDER BY {c} {d}, id LIMIT {lim}'))
        q.append((f'ties_{c}', f'SELECT {c} FROM ct ORDER BY {c} LIMIT 50'))
        q.append((f'ties_{c}', f'SELECT {c} FROM ct ORDER BY {c} DESC LIMIT 50'))
    for c, d in (('ts', 'DESC'), ('u', 'ASC'), ('k', 'DESC'), ('s', 'ASC')):
        for off in (5, 40, 2048, 5000, n // 2, int(n * 0.9)):
            for lim in (10, 100):
                q.append((f'offset_{"small" if off < 2048 else "deep"}_{c}', f'SELECT id, {c} FROM ct ORDER BY {c} {d}, id LIMIT {lim} OFFSET {off}'))
    for c in ('ts', 'u', 'k', 's'):
        for d in ('ASC', 'DESC'):
            for lim, off in ((1, 0), (20, 0), (500, 0), (20, 40), (20, 2048), (20, n // 2)):
                q.append((f'key_{c}', f'SELECT {c} FROM ct ORDER BY {c} {d} LIMIT {lim}' + (f' OFFSET {off}' if off else '')))
    for lo, hi in ((0, 5000), (400000, 450000), (999000, 1000003)):
        q.append(('range_u', f'SELECT id, u FROM ct WHERE u BETWEEN {lo} AND {hi} ORDER BY u, id LIMIT 30'))
        q.append(('range_u', f'SELECT id, u FROM ct WHERE u BETWEEN {lo} AND {hi} ORDER BY u DESC, id LIMIT 30 OFFSET 100'))
    q.append(('range_ts', f"SELECT id, ts FROM ct WHERE ts >= TIMESTAMP '2026-01-01' + interval '{n // 2} minutes' ORDER BY ts, id LIMIT 20"))
    q.append(('range_ts', f"SELECT id, ts FROM ct WHERE ts < TIMESTAMP '2026-01-01' + interval '{n // 3} minutes' ORDER BY ts DESC, id LIMIT 20 OFFSET 2100"))
    q.append(('range_k', 'SELECT id, k FROM ct WHERE k BETWEEN 100 AND 140 ORDER BY k, id LIMIT 100 OFFSET 2048'))
    q.append(('range_k', 'SELECT id, k FROM ct WHERE k >= 250 ORDER BY k DESC, id LIMIT 100'))
    q.append(('range_s', "SELECT id, s FROM ct WHERE s BETWEEN 'a00' AND 'a80' ORDER BY s, id LIMIT 50"))
    q.append(('range_s', "SELECT id, s FROM ct WHERE s >= 'f00' ORDER BY s DESC, id LIMIT 50 OFFSET 2048"))
    us = ','.join(str((i * 104729) % 1000003) for i in range(1, n, max(1, n // 40)))
    q.append(('in_u', f'SELECT id, u FROM ct WHERE u IN ({us}) ORDER BY u, id LIMIT 100'))
    q.append(('in_u', f'SELECT id, u FROM ct WHERE u IN ({us}) ORDER BY u DESC, id LIMIT 10 OFFSET 20'))
    q.append(('in_k', 'SELECT id, k FROM ct WHERE k IN (3, 77, 120, 299) ORDER BY k, id LIMIT 200 OFFSET 2048'))
    q.append(('in_k', 'SELECT id, k FROM ct WHERE k IN (3, 77, 120, 299) ORDER BY k DESC, id LIMIT 200'))
    q.append(('in_u1', f'SELECT u FROM ct WHERE u IN ({us}) ORDER BY u LIMIT 100'))
    q.append(('in_u1', f'SELECT u FROM ct WHERE u IN ({us}) ORDER BY u DESC LIMIT 10 OFFSET 3'))
    ks = ','.join(str(x) for x in range(3, 300, 7))
    q.append(('in_k1', f'SELECT k FROM ct WHERE k IN ({ks}) ORDER BY k LIMIT 300 OFFSET 2048'))
    q.append(('in_k1', f'SELECT k FROM ct WHERE k IN ({ks}) AND k > 100 ORDER BY k DESC LIMIT 100'))
    q.append(('in_k1', 'SELECT k FROM ct WHERE k IN (3, 3, 77) ORDER BY k LIMIT 400'))
    ss = ','.join(f"'{x:03x}'" for x in range(0, 4096, 97))
    q.append(('in_s', f'SELECT id, s FROM ct WHERE s IN ({ss}) ORDER BY s, id LIMIT 100'))
    for g in (3, 7, 49):
        for d in ('ASC', 'DESC'):
            for off in (0, 50, 500):
                q.append(('group_ts', f'SELECT id, ts FROM ct WHERE g = {g} ORDER BY ts {d}, id LIMIT 10 OFFSET {off}'))
    q.append(('group_ts', 'SELECT id, ts FROM ct WHERE g IN (1, 2, 3) ORDER BY ts DESC, id LIMIT 20'))
    for nl in ('NULLS FIRST', 'NULLS LAST'):
        for d in ('ASC', 'DESC'):
            for off in (0, 2048, int(n * 0.9)):
                q.append(('null_k', f'SELECT id, k FROM ct ORDER BY k {d} {nl}, id LIMIT 30 OFFSET {off}'))
    return q


class Sess:
    """A psql session: send a script, read to the sentinel."""

    def __init__(self, name, log, setup=''):
        self.name, self.log, self.n = name, log, 0
        env = dict(os.environ, LC_ALL='C', PGHOST=SOCK, PGPORT=PORT)
        for k in ('PGDATABASE', 'PGSERVICE', 'PGOPTIONS'):
            env.pop(k, None)
        self.p = subprocess.Popen(['psql', '-X', '-A', '-t', '-F', '|', '-U', 'postgres', '-d', DB], stdin=subprocess.PIPE,
                                  stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, env=env)
        if setup:
            self.run(setup)

    def run(self, sql, timeout=900):
        self.n += 1
        tok = f'@@END {self.n}'
        self.log.write(f'-- {self.name}\n{sql.strip()}\n')
        self.p.stdin.write(sql.rstrip() + f'\n\\echo {tok}\n')
        self.p.stdin.flush()
        out = []
        while True:
            line = self.p.stdout.readline()
            if not line:
                raise RuntimeError(f'{self.name}: psql exited: {"".join(out)[-500:]}')
            if line.strip() == tok:
                break
            out.append(line)
        out = ''.join(out)
        self.log.write(out)
        self.log.flush()
        return out

    def close(self):
        try:
            self.p.stdin.close()
            self.p.wait(timeout=10)
        except Exception:
            self.p.kill()


class Fail(Exception):
    pass


SETUP = ("SET client_min_messages = warning; SET synchronous_commit = on; SET sift.async_maintenance = off; SET max_parallel_maintenance_workers = 0; "
         "SET max_parallel_workers_per_gather = 0; SET lock_timeout = '30s';")


class Churn:
    def __init__(self, rng, n):
        self.rng, self.n, self.next = rng, n, n + 1

    def window(self, pct):
        r = self.rng
        pat = r.choice(['random', 'new', 'old'])
        if pat == 'random':
            m = r.randint(1, 1000)
            return f'(id::bigint * {r.choice([2654435761, 40503, 7919])} + {m}) % 1000 < {pct * 10}'
        hi = self.next - 1
        return f'id > {int(hi * (100 - pct) / 100)}' if pat == 'new' else f'id <= {int(hi * pct / 100)}'

    def step(self, reader):
        r = self.rng
        kind = r.choices(['ins', 'del', 'upd_key', 'upd_pad', 'upsert', 'aborted', 'chain', 'promote', 'vacuum', 'merge', 'reindex', 'analyze', 'fold'],
                         [14, 14, 14, 8, 8, 6, 6, 8, 10, 8, 2, 2, 10])[0]
        pct = r.choice([1, 3, 10, 30, 60])
        if kind == 'reindex' and reader:   # its lock waits for the reader's transaction
            kind = 'analyze'
        if kind == 'ins':
            a, m = self.next, r.choice([50, 500, 3000])
            self.next += m
            ts = r.choice([f"TIMESTAMP '2026-01-01' + i * interval '1 minute'", f"TIMESTAMP '2026-01-01' + ((i * 7919) % {self.n}) * interval '1 minute'"])
            return kind, (f"INSERT INTO ct SELECT i, CASE WHEN i % 11 = 0 THEN NULL ELSE (i * 7919) % 300 END, (i * 104729::bigint) % 1000003, {ts}, "
                          f"lpad(to_hex((i * 40503::bigint) % 4096), 3, '0'), i % 50, repeat('x', 60) FROM generate_series({a}, {a + m - 1}) i")
        if kind == 'del':
            return kind, f'DELETE FROM ct WHERE {self.window(pct)}'
        if kind == 'upd_key':
            return kind, (f"UPDATE ct SET ts = ts + interval '3 hours', u = (u + 5000) % 1000003, k = (k + 1) % 300, g = (g + 1) % 50, "
                          f"s = lpad(to_hex((id * 7) % 4096), 3, '0') WHERE {self.window(pct)}")
        if kind == 'upd_pad':
            return kind, f"UPDATE ct SET pad = repeat('y', {r.randint(10, 100)}) WHERE {self.window(pct)}"
        if kind == 'upsert':
            a = r.randint(1, self.next - 1)
            return kind, (f"INSERT INTO ct SELECT i, 5, i, TIMESTAMP '2027-01-01' + i * interval '1 second', 'zzz', 1, 'u' FROM generate_series({a}, {a + 500}) i "
                          f"ON CONFLICT (id) DO UPDATE SET k = excluded.k + 1, ts = excluded.ts")
        if kind == 'aborted':
            return kind, f'BEGIN; DELETE FROM ct WHERE {self.window(pct)}; UPDATE ct SET u = u + 1 WHERE {self.window(pct)}; ROLLBACK'
        if kind == 'chain':
            w = self.window(5)
            return kind, ' ; '.join(f"UPDATE ct SET ts = ts + interval '{i} minutes', k = (k + {i}) % 300 WHERE {w}" for i in (1, 2, 3))
        if kind == 'promote':
            return kind, "SELECT sift.col_promote('ct_i'), sift.col_promote('ct_g')"
        if kind == 'vacuum':
            return kind, 'VACUUM ct'
        if kind == 'fold':
            return kind, "SELECT sift._test_col_fold_marks('ct_i'), sift._test_col_fold_marks('ct_g')"
        if kind == 'merge':
            return kind, "SELECT sift.col_merge('ct_i', true), sift.col_merge('ct_g', true)"
        if kind == 'reindex':
            return kind, 'REINDEX INDEX ct_i'
        return kind, 'ANALYZE ct'


def check(a, label, every, off):
    out = a.run(f"SELECT ct_run({sqlstr(label)}, {every}, {off});\nSELECT 'verify', ct_verify('ct_i'), ct_verify('ct_g');\n"
                f"SELECT 'bad', stage, qid, class, left(detail, 300) FROM ct_res WHERE NOT ok;\nTRUNCATE ct_res;")
    if ERR.search(out):
        raise Fail(f'{label}: {out.strip()[:1500]}')
    for line in out.split('\n'):
        if line.startswith('verify|') and line != 'verify|ok|ok':
            raise Fail(f'{label}: col_verify: {line}')
        if line.startswith('bad|'):
            raise Fail(f'{label}: answers differ: {line}')


def run_seed(seed, n, steps):
    rng = random.Random(seed)
    os.makedirs(OUT, exist_ok=True)
    path = f'{OUT}/seed_{seed}.log'
    log = open(path, 'w')
    seek = rng.random() < 0.5
    cfg = dict(track=rng.choice(['on', 'on', 'off']), key_cut=rng.choice(['off', 'auto']), seg_kb=rng.choice([48, 96, 200]), seek=seek, wbuf=rng.choice(['on', 'on', 'off']))
    log.write(f'-- seed {seed} {cfg}\n')
    n = rng.choice([n // 2, n])
    keys = rng.choice([('(i * 7919) % 300', 'few'), ('(i * 7919) % 5', 'tiny'), ('(i * 7919) % 50000', 'many')])
    trk = ", track_changes = 'off'" if cfg['track'] == 'off' else ''
    kc = '' if cfg['key_cut'] == 'auto' else ", key_cut = 'off'"
    ts = rng.choice(["TIMESTAMP '2026-01-01' + i * interval '1 minute'", f"TIMESTAMP '2026-01-01' + ((i * 7919) % {n}) * interval '1 minute'"])
    flags = f"SET sift.test_col_segment_kb = {cfg['seg_kb']}; SET sift.col_wbuf_ref = {cfg['wbuf']};" + (' SET sift.test_col_gorder_seek = on;' if cfg['seek'] else '')
    a, rd = Sess('writer', log, SETUP), Sess('reader', log, SETUP)
    try:
        a.run(f"{flags}\nDROP TABLE IF EXISTS ct, ct_q, ct_res CASCADE;\n" + LIB.replace('CREATE FUNCTION', 'CREATE OR REPLACE FUNCTION'))
        ins = ',\n'.join(f'({i}, {sqlstr(c)}, {sqlstr(q)})' for i, (c, q) in enumerate(queries(n), 1))
        out = a.run(f"""INSERT INTO ct_q VALUES {ins};
CREATE TABLE ct (id int PRIMARY KEY, k int, u bigint, ts timestamp, s text, g int, pad text) WITH (autovacuum_enabled = off);
INSERT INTO ct SELECT i, CASE WHEN i % 11 = 0 THEN NULL ELSE {keys[0]} END, (i * 104729::bigint) % 1000003, {ts},
	lpad(to_hex((i * 40503::bigint) % 4096), 3, '0'), i % 50, repeat('x', 60) FROM generate_series(1, {n}) i;
CREATE INDEX ct_i ON ct USING sift_col (id, k, u, ts, s, g) WITH (order_array_columns = 'ts, u, k, s'{kc}{trk});
CREATE INDEX ct_g ON ct USING sift_col (id, g, ts) WITH (order_array_columns = 'ts', order_array_group = 'g'{kc}{trk});
CREATE INDEX ct_b1 ON ct (k, id); CREATE INDEX ct_b2 ON ct (u, id); CREATE INDEX ct_b3 ON ct (ts, id); CREATE INDEX ct_b4 ON ct (s, id); CREATE INDEX ct_b5 ON ct (g, ts, id);
VACUUM (ANALYZE) ct;""")
        if ERR.search(out):
            raise Fail(f'setup: {out.strip()[:1500]}')
        c = Churn(rng, n)
        check(a, 'base', 4, 0)
        reader = rng.random() < 0.6
        if reader:
            out = rd.run("BEGIN ISOLATION LEVEL REPEATABLE READ;\nSELECT 'snap', ct_snap(7, 0);")
            if not re.search(r'^snap\|$', out, re.M):
                raise Fail(f'reader first read: {out.strip()[:500]}')
        for i in range(steps):
            kind, sql = c.step(reader)
            out = a.run(sql + ';')
            if ERR.search(out):
                raise Fail(f'step {i} {kind}: {sql[:200]}: {out.strip()[:800]}')
            check(a, f'step {i} {kind}', 3, rng.randint(0, 2))
            if reader:
                out = rd.run("SELECT 'snap', ct_snap(7, 0);")
                if ERR.search(out) or not re.search(r'^snap\|$', out, re.M):
                    raise Fail(f'step {i} {kind}: reader snapshot changed: {out.strip()[:800]}')
        if reader:
            rd.run('COMMIT;')
        a.run('VACUUM ct;')
        check(a, 'final', 2, 0)
        a.run('DROP TABLE ct, ct_q, ct_res CASCADE;')
        log.write('-- OK\n')
        return 0, path
    except Fail as e:
        log.write(f'-- FAIL {e}\n')
        print(f'seed={seed} FAIL: {e}', flush=True)
        return 1, path
    finally:
        a.close(); rd.close(); log.close()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--seeds', default='1-3')
    ap.add_argument('--rows', type=int, default=30000)
    ap.add_argument('--steps', type=int, default=10)
    o = ap.parse_args()
    lo, _, hi = o.seeds.partition('-')
    bad = 0
    for seed in range(int(lo), int(hi or lo) + 1):
        t0 = time.time()
        rc, path = run_seed(seed, o.rows, o.steps)
        print(f'seed={seed} {"OK" if rc == 0 else "FAIL"} {time.time() - t0:.0f}s log={path}', flush=True)
        bad += rc
    sys.exit(1 if bad else 0)


if __name__ == '__main__':
    main()
