-- sum and avg of an int2 or int4 expression of more than one operation (smallint * 3 + 1, (smallint + 1) * 3) are the exact sum of the expression, a bigint for the sum
-- (PostgreSQL's int4_sum) and a numeric for the average. One operation on a smallint column and an integer constant is the program of an int8 column (test
-- agg_int2_sum_const); the digits of the expression prove that no row leaves the type here. An expression that can (smallint * 100000, an int4 column + 1) stays with
-- PostgreSQL's own functions, and raises what they raise. The results, errors included, are those of the normal plan.
CREATE EXTENSION pg_sift;
SET max_parallel_workers_per_gather = 0;
SET seq_page_cost = 10000;
SET random_page_cost = 10000;

CREATE TABLE si (id int, g int, s smallint, i int, n smallint) WITH (autovacuum_enabled = false);
INSERT INTO si SELECT x, x % 7, ((x::bigint * 7919) % 65536 - 32768)::smallint, ((x::bigint * 104729) % 4000000 - 2000000)::int, CASE WHEN x % 11 = 0 THEN NULL ELSE (x % 200 - 100)::smallint END
FROM generate_series(1, 150000) x;
INSERT INTO si VALUES (200001, 0, -32768, 2147483647, -32768), (200002, 0, 32767, -2147483648, 32767), (200003, 1, NULL, NULL, NULL), (200004, 1, 0, 0, 0);
CREATE INDEX si_col ON si USING sift_col (id, g, s, i, n);
VACUUM (FREEZE, ANALYZE) si;
CREATE TABLE sx AS SELECT * FROM si WHERE id > 200000;
CREATE INDEX sx_col ON sx USING sift_col (id, g, s, i, n);
VACUUM (FREEZE, ANALYZE) sx;

CREATE FUNCTION answer(q text, scan bool) RETURNS text LANGUAGE plpgsql AS $$
DECLARE r text;
BEGIN
	EXECUTE format('SET LOCAL sift.enable_agg = %s', CASE WHEN scan THEN 'on' ELSE 'off' END);
	BEGIN
		EXECUTE format('SELECT coalesce(string_agg(r::text, E''\n'' ORDER BY r::text), '''') FROM (%s) r', q) INTO r;
	EXCEPTION WHEN OTHERS THEN
		r := 'ERROR: ' || SQLERRM;
	END;
	RETURN r;
END $$;
CREATE FUNCTION uses_scan(q text) RETURNS bool LANGUAGE plpgsql AS $$
DECLARE l text; s bool := false;
BEGIN
	SET LOCAL sift.enable_agg = on;
	SET LOCAL sift.agg_force = on;
	FOR l IN EXECUTE 'EXPLAIN (COSTS OFF) ' || q LOOP
		s := s OR l LIKE '%Custom Scan (SiftAgg)%';
	END LOOP;
	RETURN s;
END $$;

CREATE TABLE si_q (q text);
INSERT INTO si_q SELECT format(q, t) FROM (VALUES
	('SELECT sum((s + 1) * 3), sum(s * 3 + 1), sum(1 + s * 2), sum((s + 1) + 5), sum(n * 3 + 1), sum(100 - (n + 1)) FROM %s'),
	('SELECT avg((s + 1) * 3), avg(s * 3 + 1), avg(n * 2 - 5), count(*) FROM %s'),
	('SELECT g, sum((s + 7) * 2), sum(s * 2 + 1), avg(s * 2 + 1) FROM %s GROUP BY g'),
	('SELECT sum((s + 1) * 3) FILTER (WHERE g = 2), sum(s * 3 + 1) FILTER (WHERE n > 0) FROM %s'),
	('SELECT sum((s + 1) * 3), avg(s * 3 + 1) FROM %s WHERE id BETWEEN 1000 AND 90000'),
	('SELECT sum((s + 1) * 65535) FROM %s'),
	('SELECT sum((s + 1) * 100000) FROM %s'),
	('SELECT sum(s * 100000) FROM %s'),
	('SELECT sum(s * s) FROM %s'),
	('SELECT sum(i + 1) FROM %s'),
	('SELECT sum(n + n) FROM %s'),
	('SELECT sum((s + 2147450000) * 2) FROM %s'),
	('SELECT sum((s + 1) * 3) FROM %s WHERE id = 200003'),
	('SELECT sum((s + 1) * 3), avg(s * 3 + 1) FROM %s WHERE false'),
	('SELECT sum((s + 1) * 3), sum((s + 2) * 3), sum((s + 3) * 3), sum((s + 4) * 3), sum((s + 5) * 3), sum((s + 6) * 3), sum((s + 7) * 3), sum((s + 8) * 3) FROM %s')
	) q(q), (VALUES ('si'), ('sx')) t(t);

SELECT count(*) AS statements, count(*) FILTER (WHERE answer(q, true) = answer(q, false)) AS same, count(*) FILTER (WHERE uses_scan(q)) > count(*) / 2 AS mostly_scanned FROM si_q;
SELECT q FROM si_q WHERE answer(q, true) <> answer(q, false);

SET sift.agg_force = on;
SELECT sum((s + 1) * 3), sum(s * 3 + 1), avg((s + 1) * 3), sum(n * 3 + 100) FROM si;
SELECT sum(s * 100000) FROM sx;
SELECT sum((s + 1) * 100000) FROM sx;
EXPLAIN (COSTS OFF) SELECT sum((s + 1) * 3), avg(s * 3 + 1) FROM si;
RESET sift.agg_force;

DROP FUNCTION answer(text, bool);
DROP FUNCTION uses_scan(text);
DROP TABLE si_q, sx, si;
DROP EXTENSION pg_sift;
