-- A row comparison (a, b) > (x, y), the page of a keyset pagination, is the comparisons it means: a > x OR (a = x AND b > y), the last pair with the operator itself, so the
-- zones and summaries test its leaves and a top-N reads the units from the key on. Its rows are those of the normal plan for each operator, for NULLs in the
-- columns, for two and three columns, for constants and for parameters, and the granules a top-N reads from a key do not grow with the granules before it.
CREATE EXTENSION pg_sift;
SET client_min_messages = warning;
SET max_parallel_maintenance_workers = 0;
SET max_parallel_workers_per_gather = 0;
SET seq_page_cost = 10000; SET random_page_cost = 10000;

CREATE TABLE rc (id int, a int, b int, c int, pad text) WITH (fillfactor = 10, autovacuum_enabled = false);
INSERT INTO rc SELECT i, CASE WHEN i % 31 = 0 THEN NULL ELSE (i / 256) % 7 END, CASE WHEN i % 17 = 0 THEN NULL ELSE i % 5 END, i % 3, repeat('x', 600)
FROM generate_series(0, 40 * 256 - 1) i;
CREATE INDEX rc_idx ON rc USING sift_col (id sift.col_int4_ops, a sift.col_int4_ops, b sift.col_int4_ops, c sift.col_int4_ops);
VACUUM (FREEZE, ANALYZE) rc;

CREATE FUNCTION same(cond text) RETURNS TABLE (engaged_or_empty bool, result_rows bigint, vs_normal_plan bigint) LANGUAGE plpgsql AS $$
DECLARE q text := format('SELECT count(*) AS n, sum(c) AS s, sum(id) AS i FROM rc WHERE %s', cond);
	l text;
BEGIN
	engaged_or_empty := false;
	FOR l IN EXECUTE 'EXPLAIN (COSTS OFF) ' || q LOOP
		engaged_or_empty := engaged_or_empty OR l LIKE '%Custom Scan (SiftAgg%';
	END LOOP;
	EXECUTE format('CREATE TEMP TABLE r_on AS %s', q);
	SET LOCAL sift.enable_agg = off;
	EXECUTE format('CREATE TEMP TABLE r_off AS %s', q);
	RESET sift.enable_agg;
	result_rows := (SELECT n FROM r_on);
	engaged_or_empty := engaged_or_empty OR result_rows = 0;   -- no row can match (a key that is not in the table): whether PostgreSQL 19 still plans the scan is not what is tested
	vs_normal_plan := (SELECT count(*) FROM ((SELECT * FROM r_on EXCEPT ALL SELECT * FROM r_off) UNION ALL (SELECT * FROM r_off EXCEPT ALL SELECT * FROM r_on)) d);
	DROP TABLE r_on, r_off;
	RETURN NEXT;
END $$;

CREATE TABLE rc_q (n serial, cond text);
INSERT INTO rc_q (cond) SELECT format('(%s) %s (%s)', cols, op, vals)
FROM (VALUES ('a, b', '3, 2'), ('a, b', '0, 0'), ('a, b', '6, 4'), ('b, a', '2, 5'), ('a, b, c', '3, 2, 1'), ('a, b, c', '2, NULL, 1'), ('id, a', '5000, 3'), ('id, b', '10239, 2')) k(cols, vals),
	 (VALUES ('>'), ('>='), ('<'), ('<='), ('<>'), ('=')) o(op);

SELECT n, cond, s.* FROM rc_q, LATERAL same(cond) s ORDER BY n;

-- a parameter in a generic plan
SET plan_cache_mode = force_generic_plan;
PREPARE kp(int, int) AS SELECT count(*), sum(id) FROM rc WHERE (a, b) >= ($1, $2);
CREATE TEMP TABLE kp_on AS EXECUTE kp(3, 2);
SET sift.enable_agg = off;
CREATE TEMP TABLE kp_off AS EXECUTE kp(3, 2);
RESET sift.enable_agg;
SELECT (SELECT count(*) FROM ((SELECT * FROM kp_on EXCEPT ALL SELECT * FROM kp_off) UNION ALL (SELECT * FROM kp_off EXCEPT ALL SELECT * FROM kp_on)) d) AS param_differences, (SELECT count FROM kp_on) AS param_rows;
DEALLOCATE kp;
RESET plan_cache_mode;

-- the keyset page of a top-N: the same rows as the normal plan, from the key on, and the granules read with the key far in the table and near its start (the scan forced:
-- at 10000 a page, the heap rows the top-N fetches price it above the index-only scan of the index)
CREATE FUNCTION page_granules(key int, n int) RETURNS TABLE (granules int, rows_ok bool) LANGUAGE plpgsql AS $$
DECLARE l text; u int; q text := format('SELECT id FROM rc WHERE (id, a) > (%s, 0) ORDER BY id, a LIMIT %s', key, n);
	got int[]; want int[];
BEGIN
	SET LOCAL sift.agg_force = on;
	FOR l IN EXECUTE 'EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS OFF) ' || q LOOP
		u := coalesce(substring(l from 'Granules: scanned=(\d+)')::int, u);
	END LOOP;
	EXECUTE 'SELECT array_agg(id ORDER BY id) FROM (' || q || ') x' INTO got;
	SET LOCAL sift.enable_agg = off;
	EXECUTE 'SELECT array_agg(id ORDER BY id) FROM (' || q || ') x' INTO want;
	RESET sift.enable_agg;
	RESET sift.agg_force;
	granules := u; rows_ok := got = want;
	RETURN NEXT;
END $$;
SELECT f.granules <= n.granules AS far_reads_no_more, n.rows_ok AS near_rows_ok, f.rows_ok AS far_rows_ok FROM page_granules(100, 10) n, page_granules(9000, 10) f;

-- the leading column of a row comparison, the column of a coalesce whose default fails and of a nullif, and IS NOT DISTINCT FROM a constant prune the granules of the scan as the
-- comparison of the column does (an expression of the column prunes none), and the rows are those of the normal plan
CREATE FUNCTION scanned(cond text) RETURNS int LANGUAGE plpgsql AS $$
DECLARE l text; g int;
BEGIN
	FOR l IN EXECUTE 'EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS OFF) SELECT sum(c) FROM rc WHERE ' || cond LOOP
		g := coalesce(substring(l from 'Granules: scanned=(\d+)')::int, g);
	END LOOP;
	RETURN g;
END $$;
SELECT cond, scanned(cond) < scanned('(id + 0, a) > (9000, 0)') AS pruned, s.engaged_or_empty, s.vs_normal_plan
FROM unnest(ARRAY['(id, a) > (9000, 0)', '(id, b) <= (300, 2)', 'coalesce(id, -1) > 9000', 'nullif(id, 3) < 300', 'id IS NOT DISTINCT FROM 9000']) WITH ORDINALITY u(cond, i),
	LATERAL same(cond) s ORDER BY i;
DROP FUNCTION page_granules(int, int), same(text), scanned(text);
DROP TABLE rc_q, rc;
DROP EXTENSION pg_sift;
