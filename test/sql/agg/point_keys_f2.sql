-- Format 2: Custom Scan (SiftAgg) reads the point directory of a part (col/f2/point_dir.h) for an equality or a short list on a key whose values are nearly all different, and for the keys of a
-- lookup join (inner, IN, EXISTS) of a few dimension rows: it decodes the granules of the rows the directory names ("from point directory" of EXPLAIN ANALYZE). Every conjunct still runs on
-- those rows: the rows are those of the normal plan, before and after writes, and in parallel workers.
CREATE EXTENSION pg_sift;
SET client_min_messages = warning;
SET sift.test_create_format1 = off;
SET max_parallel_workers_per_gather = 0;
SET sift.async_maintenance = off;
SET max_parallel_maintenance_workers = 0;

-- u and t are keys of one row each; the floats make the directory a small share of the bytes of the part
CREATE TABLE pf (id int, u bigint, t text COLLATE "C", g int, v float8, f1 float8, f2 float8, f3 float8, f4 float8, f5 float8, f6 float8, f7 float8, f8 float8);
INSERT INTO pf SELECT i, (i::bigint * 7919) % 1000003, 'user-' || md5('t' || i), i % 7, i % 100, sin(i + 1), sin(i + 2), sin(i + 3), sin(i + 4), sin(i + 5), sin(i + 6), sin(i + 7), sin(i + 8)
FROM generate_series(1, 40000) i;
CREATE INDEX pf_col ON pf USING sift_col (id, u, t, g, v, f1, f2, f3, f4, f5, f6, f7, f8);
CREATE TABLE dim (u bigint, grp int);
INSERT INTO dim SELECT (g::bigint * 7919 * 997) % 1000003, g % 3 FROM generate_series(1, 40) g;
INSERT INTO dim VALUES (NULL, 1), (123456789, 2);
VACUUM (FREEZE, ANALYZE) pf, dim;

CREATE FUNCTION plan_of(q text) RETURNS text LANGUAGE plpgsql AS $$
DECLARE l text; r text := 'normal';
BEGIN
	FOR l IN EXECUTE 'EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS OFF) ' || q LOOP
		IF l LIKE '%Custom Scan (SiftAgg%' THEN r := 'SiftAgg'; END IF;
		IF l LIKE '%from point directory%' THEN r := r || ' directory'; END IF;
	END LOOP;
	RETURN r;
END $$;

CREATE FUNCTION same(q text) RETURNS TABLE (plan text, result_rows bigint, vs_normal_plan bigint) LANGUAGE plpgsql AS $$
BEGIN
	plan := plan_of(q);
	EXECUTE format('CREATE TEMP TABLE r_on ON COMMIT DROP AS %s', q);
	SET LOCAL sift.enable_agg = off;
	EXECUTE format('CREATE TEMP TABLE r_off ON COMMIT DROP AS %s', q);
	RESET sift.enable_agg;
	result_rows := (SELECT count(*) FROM r_on);
	vs_normal_plan := (SELECT count(*) FROM ((TABLE r_on EXCEPT TABLE r_off) UNION ALL (TABLE r_off EXCEPT TABLE r_on)) d);
	DROP TABLE r_on, r_off;
	RETURN NEXT;
END $$;

-- an equality, IN lists (NULL among them, a key the table lacks), text keys, a key and another conjunct, the keys of an IN, a join and an EXISTS of the dimension; a range and a NOT IN read no directory
CREATE FUNCTION all_same() RETURNS TABLE (n int, plan text, result_rows bigint, vs_normal_plan bigint) LANGUAGE plpgsql AS $$
DECLARE q text[] := ARRAY[
	'SELECT count(*), sum(v) FROM pf WHERE u = 7919',
	'SELECT count(*), sum(v), min(id) FROM pf WHERE u IN (7919, 15838, 23757, 5, 999999)',
	'SELECT count(*) FROM pf WHERE u IN (7919, NULL, 15838)',
	'SELECT g, count(*), sum(v) FROM pf WHERE t IN (''user-' || md5('t5') || ''', ''user-' || md5('t77') || ''', ''nosuch'') GROUP BY g ORDER BY g',
	'SELECT count(*), sum(v) FROM pf WHERE u IN (7919, 15838, 23757) AND g < 3',
	'SELECT count(*), sum(v) FROM pf WHERE u IN (SELECT u FROM dim WHERE grp = 1)',
	'SELECT dim.grp, count(*), sum(v) FROM pf JOIN dim ON dim.u = pf.u GROUP BY 1 ORDER BY 1',
	'SELECT count(*) FROM pf WHERE EXISTS (SELECT 1 FROM dim WHERE dim.u = pf.u AND dim.grp = 2)',
	'SELECT count(*), sum(v) FROM pf WHERE u BETWEEN 100 AND 5000',
	'SELECT count(*) FROM pf WHERE u NOT IN (1, 2, 3)'];
BEGIN
	FOR n IN 1..array_length(q, 1) LOOP
		RETURN QUERY SELECT n, s.plan, s.result_rows, s.vs_normal_plan FROM same(q[n]) s;
	END LOOP;
END $$;

SET sift.agg_force = on;
SELECT 'built' AS state, * FROM all_same();

-- parallel workers each read their own granules of the part
SET parallel_setup_cost = 0; SET parallel_tuple_cost = 0; SET min_parallel_table_scan_size = 0; SET max_parallel_workers_per_gather = 2;
SELECT 'parallel' AS state, * FROM all_same();
RESET max_parallel_workers_per_gather;

-- the changed rows are in the write buffer and the rows they replace are dead in the part, which then holds column deltas: a part whose values a delta patches is read
-- without its directory (col/f2/scan/scan.c part_open), inside the transaction and after it
BEGIN;
UPDATE pf SET u = u + 1 WHERE id % 97 = 0;
DELETE FROM pf WHERE id % 89 = 0;
INSERT INTO pf SELECT id + 100000, u, t, g, v, f1, f2, f3, f4, f5, f6, f7, f8 FROM pf WHERE id % 211 = 0;
SELECT 'in transaction' AS state, * FROM all_same();
COMMIT;
SELECT 'committed' AS state, * FROM all_same();

RESET ALL;
DROP TABLE pf, dim;
DROP FUNCTION all_same(), same(text), plan_of(text);
DROP EXTENSION pg_sift;
