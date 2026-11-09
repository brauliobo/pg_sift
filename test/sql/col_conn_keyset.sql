-- The fill of the found flags holds the keys of the parent in the least memory exact membership takes (a bitmap of a dense range, a sorted array of a sparse one), not 48 bytes a key:
-- with maintenance_work_mem = 1MB (the fill may use half) a dense parent of 100,000 keys and a sparse one of 30,000 are resolved, which a table of 48 bytes a key refused at
-- about 10,900 keys. The rows are those of the normal plan, and a parent whose sparse keys need more is still refused.
CREATE EXTENSION pg_sift;
CREATE EXTENSION pg_sift_test;
SET max_parallel_workers_per_gather = 0;
SET jit = off;
SET sift.agg_force = on;
SET sift.agg_lookup_cache_kb = 0;
SET sift.agg_lookup_shared_kb = 0;
SET maintenance_work_mem = '1MB';
SET client_min_messages = warning;

CREATE FUNCTION found_run(q text, OUT same boolean, OUT proofs int, OUT why text) LANGUAGE plpgsql AS $$
DECLARE
	p jsonb;
	a text;
	b text;
BEGIN
	SET LOCAL sift.enable_agg = on;
	EXECUTE 'EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS OFF, FORMAT JSON) ' || q INTO p;
	EXECUTE 'SELECT coalesce(json_agg(t)::text, '''') FROM (' || q || ') t' INTO a;
	SET LOCAL sift.enable_agg = off;
	EXECUTE 'SELECT coalesce(json_agg(t)::text, '''') FROM (' || q || ') t' INTO b;
	SET LOCAL sift.enable_agg = on;
	same := a = b;
	proofs := coalesce(jsonb_path_query_first(p, '$.**."Found Proofs"')::text::int, 0);
	why := jsonb_path_query_first(p, '$.**."Found Fallback"') #>> '{}';
END $$;

-- parent n: dense (1..N), sparse (1000 * i), over (70,000 sparse); children: every key of the parent, some NULLs and some keys it lacks
CREATE TABLE parents (name text, n int, step int);
INSERT INTO parents VALUES ('dense', 100000, 1), ('sparse', 30000, 1000), ('over', 70000, 1000);
CREATE FUNCTION make_pair(p parents) RETURNS void LANGUAGE plpgsql AS $$
BEGIN
	EXECUTE format('CREATE TABLE dim_%1$s (id int PRIMARY KEY, label text) WITH (autovacuum_enabled = false)', p.name);
	EXECUTE format('INSERT INTO dim_%1$s SELECT i * %2$s, ''l'' || i FROM generate_series(1, %3$s) i', p.name, p.step, p.n);
	EXECUTE format('CREATE INDEX dim_%1$s_col ON dim_%1$s USING sift_col (id, label)', p.name);
	EXECUTE format('CREATE TABLE ev_%1$s (id int, dim_id int, v int) WITH (autovacuum_enabled = false)', p.name);
	EXECUTE format('INSERT INTO ev_%1$s SELECT i, CASE WHEN i %% 97 = 0 THEN NULL WHEN i %% 89 = 0 THEN -1 ELSE %2$s * (1 + i %% %3$s) END, i %% 10 FROM generate_series(1, 5000) i',
				   p.name, p.step, p.n);
	EXECUTE format('CREATE INDEX ev_%1$s_col ON ev_%1$s USING sift_col (id, dim_id, v) WITH (connect = ''dim_id -> dim_%1$s(id)'')', p.name);
END $$;
SELECT make_pair(p) FROM parents p ORDER BY p.n;

-- the build began before the journal of the parent: the next rewrite fills the flags
SELECT name, sift.col_maintain(format('ev_%s_col', name)) AS rewritten FROM parents ORDER BY n;
SELECT left(c.child::text, 20) AS child, c.fill FROM sift.col_connections c ORDER BY c.child::text;

-- the found flags answer, the rows are those of the normal plan
SELECT p.name, r.same
FROM parents p, LATERAL found_run(format($q$SELECT count(*) FROM ev_%1$s e WHERE NOT EXISTS (SELECT 1 FROM dim_%1$s d WHERE d.id = e.dim_id)$q$, p.name)) r ORDER BY p.n;
SELECT p.name, r.same
FROM parents p, LATERAL found_run(format($q$SELECT count(*) FROM ev_%1$s e JOIN dim_%1$s d ON d.id = e.dim_id$q$, p.name)) r ORDER BY p.n;
-- the parent that fits is answered from the flags (a plan that counts the keys it has), the refused one by the ordinary join
SELECT r.same, r.proofs > 0 AS flags_used FROM found_run($q$SELECT count(*) FROM ev_sparse e JOIN dim_sparse d ON d.id = e.dim_id$q$) r;
SELECT r.same, r.proofs > 0 AS flags_used FROM found_run($q$SELECT count(*) FROM ev_over e JOIN dim_over d ON d.id = e.dim_id$q$) r;

-- keys that are not evenly spread (squares), asked for keys the parent has and keys next to them it has not
CREATE TABLE dim_skew (id int PRIMARY KEY, label text) WITH (autovacuum_enabled = false);
INSERT INTO dim_skew SELECT i * i, 'l' || i FROM generate_series(1, 30000) i;
CREATE INDEX dim_skew_col ON dim_skew USING sift_col (id, label);
CREATE TABLE ev_skew (id int, dim_id int, v int) WITH (autovacuum_enabled = false);
INSERT INTO ev_skew SELECT i, CASE WHEN i % 7 = 0 THEN (1 + i % 30000) * (1 + i % 30000) + 1 ELSE (1 + i % 30000) * (1 + i % 30000) END, i % 10 FROM generate_series(1, 5000) i;
CREATE INDEX ev_skew_col ON ev_skew USING sift_col (id, dim_id, v) WITH (connect = 'dim_id -> dim_skew(id)');
SELECT sift.col_maintain('ev_skew_col') AS rewritten;
SELECT fill FROM sift.col_connections WHERE child = 'ev_skew_col'::regclass;
SELECT r.same FROM found_run($q$SELECT count(*) FROM ev_skew e WHERE NOT EXISTS (SELECT 1 FROM dim_skew d WHERE d.id = e.dim_id)$q$) r;
SELECT r.same FROM found_run($q$SELECT count(*) FROM ev_skew e JOIN dim_skew d ON d.id = e.dim_id$q$) r;

DROP FUNCTION make_pair(parents);
DROP TABLE dim_dense, ev_dense, dim_sparse, ev_sparse, dim_over, ev_over, dim_skew, ev_skew, parents;
DROP FUNCTION found_run(text);
DROP EXTENSION pg_sift_test;
DROP EXTENSION pg_sift;
