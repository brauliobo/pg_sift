-- A numeric NaN, a NULL (and, in the unconstrained numeric, an infinity) in a column of the tables a lookup or star join reads: min, max, sum, avg, count(col), count(DISTINCT col) and GROUP BY col over a numeric(p, s) and an unconstrained
-- numeric, as the column of the fact side (a self join of one relation, whose fact side the planner picks), as the payload of a dimension, and under a LEFT JOIN. Every statement runs over the heap twin (the truth),
-- the sift_col index (with a heap and a sift_table dimension) and the sift_table, serially and with 4 workers allowed; each prints whether they answer alike, and for the index the custom scan that ran it.
-- The fact tables have no unique key, so that PostgreSQL does not remove the self join.
-- requires: build col
SET client_min_messages = warning;
CREATE EXTENSION IF NOT EXISTS pg_sift;
DO $$ BEGIN PERFORM set_config('sift.test_create_format1', 'off', false); EXCEPTION WHEN OTHERS THEN NULL; END $$;
SET max_parallel_workers_per_gather = 0;
SET seq_page_cost = 10000;
SET random_page_cost = 10000;

CREATE TABLE ln_h (id int NOT NULL, g int NOT NULL, dim int, n numeric(10, 2), u numeric) WITH (autovacuum_enabled = false);
INSERT INTO ln_h
SELECT i, i % 7, 1 + i % 40,
       CASE WHEN i % 100 = 0 THEN 'NaN'::numeric WHEN i % 31 = 0 THEN NULL ELSE (i % 500) / 8.0 END,
       CASE WHEN i % 100 = 0 THEN 'NaN'::numeric WHEN i % 29 = 0 THEN NULL WHEN i % 83 = 0 THEN 'Infinity'::numeric WHEN i % 79 = 0 THEN '-Infinity'::numeric ELSE (i % 700) / 16.0 END
FROM generate_series(1, 20000) i;
CREATE TABLE ln_i (LIKE ln_h INCLUDING ALL) WITH (autovacuum_enabled = false);
CREATE TABLE ln_t (LIKE ln_h INCLUDING ALL) USING sift_table WITH (autovacuum_enabled = false);
INSERT INTO ln_i SELECT * FROM ln_h;
INSERT INTO ln_t SELECT * FROM ln_h;
CREATE INDEX ln_i_col ON ln_i USING sift_col (id, g, dim, n, u);
SELECT sift.table_maintain('ln_t') IS NOT NULL AS maintained;

-- a dimension: some keys have a NaN, an infinity, a NULL or an ordinary value in each payload; key 41 has no fact row, the fact rows of key 42 have no dimension row
CREATE TABLE ln_d (id int PRIMARY KEY, n numeric(10, 2), u numeric) WITH (autovacuum_enabled = false);
INSERT INTO ln_d
SELECT i, CASE i % 6 WHEN 0 THEN 'NaN'::numeric WHEN 1 THEN NULL ELSE i / 4.0 END,
          CASE i % 8 WHEN 0 THEN 'NaN'::numeric WHEN 1 THEN NULL WHEN 2 THEN 'Infinity'::numeric WHEN 3 THEN '-Infinity'::numeric ELSE i / 8.0 END
FROM generate_series(1, 41) i;
CREATE TABLE ln_dt (LIKE ln_d INCLUDING ALL) USING sift_table WITH (autovacuum_enabled = false);
INSERT INTO ln_dt SELECT * FROM ln_d;
SELECT sift.table_maintain('ln_dt') IS NOT NULL AS maintained;
INSERT INTO ln_h SELECT 20000 + i, i % 7, 42, i / 4.0, i / 8.0 FROM generate_series(1, 20) i;
INSERT INTO ln_i SELECT * FROM ln_h WHERE id > 20000;
INSERT INTO ln_t SELECT * FROM ln_h WHERE id > 20000;
VACUUM (FREEZE, ANALYZE) ln_h, ln_i, ln_t, ln_d, ln_dt;

CREATE FUNCTION answer(fact text, dim text, q text) RETURNS text LANGUAGE plpgsql AS $$
DECLARE r text;
BEGIN
	EXECUTE format('SELECT md5(coalesce(string_agg(r, ''|'' ORDER BY r), '''')) FROM (SELECT row_to_json(x)::text r FROM (%s) x) y', format(q, fact, dim)) INTO r;
	RETURN r;
END $$;
CREATE FUNCTION engine(q text) RETURNS text LANGUAGE plpgsql AS $$
DECLARE line text;
BEGIN
	FOR line IN EXECUTE format('EXPLAIN (COSTS OFF) %s', q) LOOP
		IF line LIKE '%Custom Scan (%' THEN RETURN substring(line FROM 'Custom Scan \((.*)\)'); END IF;
	END LOOP;
	RETURN 'none';
END $$;
CREATE TABLE cases (n serial, q text);
INSERT INTO cases (q) VALUES
 ('SELECT a.g, count(*), count(b.n), sum(b.n), avg(b.n), min(b.n), max(b.n) FROM %1$s a JOIN %1$s b ON a.id = b.id WHERE a.id BETWEEN 900 AND 1100 GROUP BY a.g'),
 ('SELECT a.g, count(*), count(b.u), sum(b.u), avg(b.u), min(b.u), max(b.u) FROM %1$s a JOIN %1$s b ON a.id = b.id WHERE a.id BETWEEN 900 AND 1100 GROUP BY a.g'),
 ('SELECT count(*), count(b.n), sum(b.n), min(b.n), max(b.n), count(DISTINCT b.n), count(DISTINCT b.u) FROM %1$s a JOIN %1$s b ON a.id = b.id WHERE a.id < 3000'),
 ('SELECT b.n, count(*) FROM %1$s a JOIN %1$s b ON a.id = b.id WHERE a.id BETWEEN 900 AND 1100 GROUP BY b.n'),
 ('SELECT b.u, count(*) FROM %1$s a JOIN %1$s b ON a.id = b.id WHERE a.id BETWEEN 900 AND 1100 GROUP BY b.u'),
 ('SELECT f.g, count(*), count(d.n), sum(d.n), avg(d.n), min(d.n), max(d.n) FROM %1$s f JOIN %2$s d ON f.dim = d.id GROUP BY f.g'),
 ('SELECT f.g, count(*), count(d.u), sum(d.u), avg(d.u), min(d.u), max(d.u) FROM %1$s f JOIN %2$s d ON f.dim = d.id GROUP BY f.g'),
 ('SELECT f.g, count(*), count(d.n), sum(d.n), min(d.n), max(d.n), count(DISTINCT d.n), count(DISTINCT d.u) FROM %1$s f LEFT JOIN %2$s d ON f.dim = d.id GROUP BY f.g'),
 ('SELECT count(d.n), sum(d.n), avg(d.n), min(d.n), max(d.n), count(d.u), sum(d.u), avg(d.u), min(d.u), max(d.u) FROM %1$s f JOIN %2$s d ON f.dim = d.id'),
 ('SELECT d.n, count(*), sum(f.id) FROM %1$s f JOIN %2$s d ON f.dim = d.id GROUP BY d.n'),
 ('SELECT d.u, count(*), min(f.n), max(f.u) FROM %1$s f LEFT JOIN %2$s d ON f.dim = d.id GROUP BY d.u'),
 ('SELECT f.g, count(*) FROM %1$s f JOIN %2$s d ON f.dim = d.id GROUP BY f.g'),
 ('SELECT f.g, min(f.n), max(f.n), min(d.n), max(d.n), sum(f.u), max(d.u) FROM %1$s f JOIN %2$s d ON f.dim = d.id WHERE d.id < 30 GROUP BY f.g');

CREATE FUNCTION report(fact text, dim text) RETURNS TABLE (n int, same boolean, engine text) LANGUAGE sql AS
$$ SELECT c.n, answer(fact, dim, c.q) = answer('ln_h', 'ln_d', c.q), engine(format(c.q, fact, dim)) FROM cases c ORDER BY c.n $$;

SELECT 'index, heap dimension' AS plan, * FROM report('ln_i', 'ln_d');
SELECT 'index, table dimension' AS plan, * FROM report('ln_i', 'ln_dt');
SELECT 'table, table dimension' AS plan, n, same FROM report('ln_t', 'ln_dt');

SET max_parallel_workers = 8;
SET max_parallel_workers_per_gather = 4;
SET parallel_setup_cost = 0;
SET parallel_tuple_cost = 0;
SET min_parallel_table_scan_size = 0;
SET min_parallel_index_scan_size = 0;
SELECT 'index, heap dimension, 4 workers' AS plan, * FROM report('ln_i', 'ln_d');
SELECT 'index, table dimension, 4 workers' AS plan, * FROM report('ln_i', 'ln_dt');
SELECT 'table, table dimension, 4 workers' AS plan, n, same FROM report('ln_t', 'ln_dt');
