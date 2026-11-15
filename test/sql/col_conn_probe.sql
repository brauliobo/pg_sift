-- The fill of the found flags knows the keys of a parent by reading it (the key set) or by asking its btree for the keys of the rows it fills (the probe), whichever is cheaper
-- (docs/COL-CONNECT.md). Both give the flags of the normal plan, on parents with deleted, updated and NULL keys, and a parent whose keys do not fit maintenance_work_mem is resolved
-- by the probe.
CREATE EXTENSION pg_sift;
CREATE EXTENSION pg_sift_test;
SET max_parallel_workers_per_gather = 0;
SET jit = off;
SET sift.agg_force = on;
SET sift.agg_lookup_cache_kb = 0;
SET sift.agg_lookup_shared_kb = 0;
SET maintenance_work_mem = '1MB';
SET client_min_messages = warning;

CREATE FUNCTION found_run(q text, OUT same boolean, OUT proofs int) LANGUAGE plpgsql AS $$
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
END $$;

-- a parent with keys that were deleted and updated to another key, and one with keys beyond int4
CREATE TABLE dim (id int PRIMARY KEY, label text) WITH (autovacuum_enabled = false);
INSERT INTO dim SELECT i * 7, 'l' || i FROM generate_series(1, 40000) i;
DELETE FROM dim WHERE id % 91 = 0;
UPDATE dim SET id = id + 1 WHERE id % 133 = 0;
CREATE INDEX dim_col ON dim USING sift_col (id, label);
CREATE TABLE dim8 (id bigint PRIMARY KEY, label text) WITH (autovacuum_enabled = false);
INSERT INTO dim8 SELECT i * 7 + 5000000000, 'l' || i FROM generate_series(1, 40000) i;
CREATE INDEX dim8_col ON dim8 USING sift_col (id, label);
-- children: keys of the parent, some it lacks (next to them), NULLs and -1
CREATE FUNCTION make_child(name text, parent text, base bigint, step int, n int, bigkey boolean) RETURNS void LANGUAGE plpgsql AS $$
BEGIN
	EXECUTE format('DROP TABLE IF EXISTS %I', name);
	EXECUTE format('CREATE TABLE %I (id int, dim_id %s, v int) WITH (autovacuum_enabled = false)', name, CASE WHEN bigkey THEN 'bigint' ELSE 'int' END);
	EXECUTE format('INSERT INTO %I SELECT i, CASE WHEN i %% 97 = 0 THEN NULL WHEN i %% 89 = 0 THEN -1 WHEN i %% 11 = 0 THEN %s + (1 + i %% %s) * %s + 1 ELSE %s + (1 + i %% %s) * %s END, i %% 10 FROM generate_series(1, 6000) i',
				   name, base, n, step, base, n, step);
	EXECUTE format('CREATE INDEX %I ON %I USING sift_col (id, dim_id, v) WITH (connect = ''dim_id -> %I(id)'')', name || '_col', name, parent);
END $$;

-- every mode fills the same flags (the same number of them, and rows that are those of the normal plan): 1 reads the parent, 2 probes its btree, 0 takes the cheaper (for 6,000 rows over 40,000 the probe)
CREATE FUNCTION report(child text, parent text) RETURNS TABLE (fill text, flags bigint, anti boolean, joined boolean) LANGUAGE sql AS $$
	SELECT (SELECT c.fill FROM sift.col_connections c WHERE c.child = (report.child || '_col')::regclass),
		   (SELECT count(val) FROM sift._test_col_read(report.child || '_col', 3)),
		   (SELECT a.same FROM found_run(format('SELECT count(*) FROM %I e WHERE NOT EXISTS (SELECT 1 FROM %I d WHERE d.id = e.dim_id)', child, parent)) a),
		   (SELECT j.same FROM found_run(format('SELECT count(*) FROM %I e JOIN %I d ON d.id = e.dim_id', child, parent)) j)
$$;
SET sift.test_conn_fill = 1;
SELECT make_child('ev', 'dim', 0, 7, 40000, false);
SELECT sift.col_maintain('ev_col') IS NOT NULL AS rewritten;
SELECT * FROM report('ev', 'dim');
SET sift.test_conn_fill = 2;
SELECT make_child('ev', 'dim', 0, 7, 40000, false);
SELECT sift.col_maintain('ev_col') IS NOT NULL AS rewritten;
SELECT * FROM report('ev', 'dim');
SET sift.test_conn_fill = 0;
SELECT make_child('ev', 'dim', 0, 7, 40000, false);
SELECT sift.col_maintain('ev_col') IS NOT NULL AS rewritten;
SELECT * FROM report('ev', 'dim');
SET sift.test_conn_fill = 1;
SELECT make_child('ev8', 'dim8', 5000000000, 7, 40000, true);
SELECT sift.col_maintain('ev8_col') IS NOT NULL AS rewritten;
SELECT * FROM report('ev8', 'dim8');
SET sift.test_conn_fill = 2;
SELECT make_child('ev8', 'dim8', 5000000000, 7, 40000, true);
SELECT sift.col_maintain('ev8_col') IS NOT NULL AS rewritten;
SELECT * FROM report('ev8', 'dim8');
SET sift.test_conn_fill = 0;
SELECT make_child('ev8', 'dim8', 5000000000, 7, 40000, true);
SELECT sift.col_maintain('ev8_col') IS NOT NULL AS rewritten;
SELECT * FROM report('ev8', 'dim8');

-- the parent changes: the next rewrite refills under the new stamp, by the same means
INSERT INTO dim VALUES (1000003, 'new');
DELETE FROM dim WHERE id = 14;
UPDATE dim SET id = 1000005 WHERE id = 21;
SET sift.test_conn_fill = 2;
SELECT sift.col_maintain('ev_col') IS NOT NULL AS refilled;
SELECT c.fill FROM sift.col_connections c WHERE c.child = 'ev_col'::regclass;
SELECT a.same FROM found_run('SELECT count(*) FROM ev e WHERE NOT EXISTS (SELECT 1 FROM dim d WHERE d.id = e.dim_id)') a;
SELECT j.same FROM found_run('SELECT count(*) FROM ev e JOIN dim d ON d.id = e.dim_id') j;

-- a parent whose keys do not fit the half of maintenance_work_mem (70,000 sparse keys need more than 512 kB) is resolved by the probe whichever mode asks for the key set
RESET sift.test_conn_fill;
CREATE TABLE dimw (id int PRIMARY KEY, label text) WITH (autovacuum_enabled = false);
INSERT INTO dimw SELECT i * 1000, 'l' || i FROM generate_series(1, 70000) i;
CREATE INDEX dimw_col ON dimw USING sift_col (id, label);
SET sift.test_conn_fill = 1;
SELECT make_child('evw', 'dimw', 0, 1000, 70000, false);
SELECT sift.col_maintain('evw_col') IS NOT NULL AS rewritten;
SELECT * FROM report('evw', 'dimw');
RESET sift.test_conn_fill;

DROP FUNCTION report(text, text);
DROP FUNCTION make_child(text, text, bigint, int, int, boolean);
DROP TABLE ev, ev8, evw, dim, dim8, dimw;
DROP FUNCTION found_run(text);
DROP EXTENSION pg_sift_test;
DROP EXTENSION pg_sift;
