-- A row comparison (a, b) > (x, y), the page of a keyset pagination, is the comparisons it means: a > x OR (a = x AND b > y), the last pair with the operator itself, so the
-- zones and summaries test its leaves and a top-N reads the units from the key on. Its rows are those of the normal plan for each operator, for NULLs in the
-- columns, for two and three columns, for constants and for parameters, and the units a top-N reads from a key do not grow with the units before it.
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

CREATE FUNCTION same(cond text) RETURNS TABLE (uses_siftagg bool, result_rows bigint, vs_normal_plan bigint) LANGUAGE plpgsql AS $$
DECLARE q text := format('SELECT count(*) AS n, sum(c) AS s, sum(id) AS i FROM rc WHERE %s', cond);
	l text;
BEGIN
	uses_siftagg := false;
	FOR l IN EXECUTE 'EXPLAIN (COSTS OFF) ' || q LOOP
		uses_siftagg := uses_siftagg OR l LIKE '%Custom Scan (SiftAgg)%';
	END LOOP;
	EXECUTE format('CREATE TEMP TABLE r_on AS %s', q);
	SET LOCAL sift.enable_agg = off;
	EXECUTE format('CREATE TEMP TABLE r_off AS %s', q);
	RESET sift.enable_agg;
	result_rows := (SELECT n FROM r_on);
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

-- the keyset page of a top-N: the same rows as the normal plan, from the key on, and the units read with the key far in the table and near its start
CREATE FUNCTION page_units(key int, n int) RETURNS TABLE (units int, rows_ok bool) LANGUAGE plpgsql AS $$
DECLARE l text; u int; q text := format('SELECT id FROM rc WHERE (id, a) > (%s, 0) ORDER BY id, a LIMIT %s', key, n);
	got int[]; want int[];
BEGIN
	FOR l IN EXECUTE 'EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS OFF) ' || q LOOP
		IF l LIKE '%Units: %' THEN u := trim(split_part(l, ':', 2))::int; END IF;
	END LOOP;
	EXECUTE 'SELECT array_agg(id ORDER BY id) FROM (' || q || ') x' INTO got;
	SET LOCAL sift.enable_agg = off;
	EXECUTE 'SELECT array_agg(id ORDER BY id) FROM (' || q || ') x' INTO want;
	RESET sift.enable_agg;
	units := u; rows_ok := got = want;
	RETURN NEXT;
END $$;
SELECT 'near the start' AS key, p.* FROM page_units(100, 10) p UNION ALL SELECT 'far in the table', p.* FROM page_units(9000, 10) p;

DROP FUNCTION page_units(int, int), same(text);
DROP TABLE rc_q, rc;
DROP EXTENSION pg_sift;
