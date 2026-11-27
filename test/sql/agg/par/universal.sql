-- The aggregate scan of an index of format major 2 on the compute threads of the backend (core/par/pool.h): GROUP BY date_bin of a time series (time zones, NULL and infinite timestamps, NULL
-- sums), star and snowflake joins (inner and left, orphans, NULL payloads, text and integer keys), and LATERAL unnest of array columns (NULL and empty arrays, NULL elements, long strings,
-- arrays longer than the index keeps, inner and left). Every query is run with the node on 4 threads and with the aggregate scan off, and the sorted rows are compared; the first word says they
-- agree, the second that the plan is the node's, the third that the answer is not empty.
CREATE EXTENSION pg_sift;
SET client_min_messages = warning;
SET max_parallel_workers_per_gather = 3;
SET jit = off;
SET work_mem = '256MB';
SET sift.test_create_format1 = off;
SET sift.agg_force = on;
\ir test/common/sql/agg/base.sql

CREATE FUNCTION node_of(q text) RETURNS bool LANGUAGE plpgsql AS $$
DECLARE l text;
BEGIN
	PERFORM set_config('sift.enable_agg', 'on', true);
	FOR l IN EXECUTE 'EXPLAIN (COSTS OFF) ' || q LOOP
		IF l LIKE '%Custom Scan (SiftAgg%' THEN RETURN true; END IF;
	END LOOP;
	RETURN false;
END $$;
CREATE FUNCTION check_par(name text, q text) RETURNS TABLE (label text, same bool, node bool, nonempty bool) LANGUAGE plpgsql AS $$
DECLARE a text := runq(q, true); b text := runq(q, false);
BEGIN
	RETURN QUERY SELECT name, a = b, node_of(q), b <> '[]' AND b NOT LIKE 'ERR%';
END $$;

-- ---- a time series ----
CREATE TABLE ts_t (id int, ts timestamptz, tn timestamp, dev int, site int, v int, w smallint) WITH (autovacuum_enabled = false);
INSERT INTO ts_t SELECT i,
	CASE WHEN i % 997 = 0 THEN NULL WHEN i % 4999 = 0 THEN 'infinity' WHEN i % 5003 = 0 THEN '-infinity' ELSE timestamptz '2024-01-01 00:00:00+00' + (i * 37 % 4000000) * interval '1 second' END,
	CASE WHEN i % 991 = 0 THEN NULL ELSE timestamp '2023-12-31 22:30:00' + (i * 53 % 4000000) * interval '1 second' END,
	i % 90, i % 5, CASE WHEN i % 20011 = 0 THEN NULL ELSE i % 1000 END, (i % 100)::smallint
-- 150000 rows are 19 granules of 8192: each of the 4 threads folds several, and the NULL and infinite instants are left to the scan
FROM generate_series(1, 150000) i;
CREATE INDEX ts_t_col ON ts_t USING sift_col (id, ts, tn, dev, site, v, w) WITH (rollup_columns = 'off');
VACUUM (FREEZE, ANALYZE) ts_t;
SELECT * FROM check_par('bin 1 hour', $q$ SELECT date_bin('1 hour', ts, timestamptz '2020-01-01') b, count(*), sum(w) FROM ts_t GROUP BY 1 $q$);
SELECT * FROM check_par('bin 15 minutes by dev', $q$ SELECT date_bin('15 minutes', ts, timestamptz '2020-01-01') b, dev, count(*), sum(w) FROM ts_t GROUP BY 1, 2 $q$);
SELECT * FROM check_par('bin 1 day by site, odd origin', $q$ SELECT date_bin('1 day', ts, timestamptz '2000-01-03 07:11:00+00') b, site, count(*), sum(w) FROM ts_t GROUP BY 1, 2 $q$);
SELECT * FROM check_par('bin of timestamp', $q$ SELECT date_bin('6 hours', tn, timestamp '2020-01-01') b, count(*), sum(w) FROM ts_t GROUP BY 1 $q$);
SELECT * FROM check_par('bin with a NULL in the sum', $q$ SELECT date_bin('1 day', ts, timestamptz '2020-01-01') b, count(*), sum(v) FROM ts_t GROUP BY 1 $q$);
SELECT * FROM check_par('bin with a predicate', $q$ SELECT date_bin('1 hour', ts, timestamptz '2020-01-01') b, count(*), sum(w) FROM ts_t WHERE dev < 40 AND ts >= timestamptz '2024-01-10' GROUP BY 1 $q$);
SET timezone = 'America/Sao_Paulo';
SELECT * FROM check_par('bin 1 day, Sao Paulo', $q$ SELECT date_bin('1 day', ts, timestamptz '2020-01-01') b, count(*), sum(w) FROM ts_t GROUP BY 1 $q$);
SET timezone = 'Asia/Kolkata';
SELECT * FROM check_par('bin 1 day, Kolkata', $q$ SELECT date_bin('1 day', ts, timestamptz '2020-01-01') b, count(*), sum(w) FROM ts_t GROUP BY 1 $q$);
RESET timezone;

-- ---- a star and a snowflake (a chain of dimensions, inner and left, is the node's too) ----
CREATE TABLE st_f (id int, d1 int, d2 int, qty int, amt int) WITH (autovacuum_enabled = false);
CREATE TABLE st_d1 (id int PRIMARY KEY, g int, name text, p int);
CREATE TABLE st_p (id int PRIMARY KEY, attr text, gp int);
CREATE TABLE st_d2 (id int, tag int);
INSERT INTO st_p SELECT i, CASE WHEN i % 7 = 0 THEN NULL ELSE 'parent-attribute-' || i % 6 END, i % 4 FROM generate_series(1, 30) i;
INSERT INTO st_d1 SELECT i, CASE WHEN i % 17 = 0 THEN NULL ELSE i % 40 END, CASE WHEN i % 23 = 0 THEN NULL WHEN i % 3 = 0 THEN 'a-long-name-of-' || i % 50 ELSE 'n' || i % 50 END, 1 + i % 35 FROM generate_series(1, 500) i;
INSERT INTO st_d2 SELECT i, i % 5 FROM generate_series(1, 200) i UNION ALL SELECT i, 100 + i % 3 FROM generate_series(1, 20) i;
-- 150000 fact rows (19 granules of 8192) against dimensions of a few hundred rows
INSERT INTO st_f SELECT i, CASE WHEN i % 41 = 0 THEN NULL ELSE 1 + (i * 7) % 600 END, 1 + (i * 11) % 230, i % 100, CASE WHEN i % 30011 = 0 THEN NULL ELSE i % 500 END FROM generate_series(1, 150000) i;
CREATE INDEX st_f_col ON st_f USING sift_col (id, d1, d2, qty, amt) WITH (rollup_columns = 'off');
VACUUM (FREEZE, ANALYZE) st_f, st_d1, st_p, st_d2;
SELECT * FROM check_par('inner, integer payload', $q$ SELECT d.g, count(*), sum(f.qty) FROM st_f f JOIN st_d1 d ON d.id = f.d1 GROUP BY 1 $q$);
SELECT * FROM check_par('left, integer payload', $q$ SELECT d.g, count(*), sum(f.qty) FROM st_f f LEFT JOIN st_d1 d ON d.id = f.d1 GROUP BY 1 $q$);
SELECT * FROM check_par('inner, text payload', $q$ SELECT d.name, count(*), sum(f.qty) FROM st_f f JOIN st_d1 d ON d.id = f.d1 GROUP BY 1 $q$);
SELECT * FROM check_par('left, text payload', $q$ SELECT d.name, count(*) FROM st_f f LEFT JOIN st_d1 d ON d.id = f.d1 GROUP BY 1 $q$);
SELECT * FROM check_par('inner, payload and fact key', $q$ SELECT d.g, f.qty, count(*) FROM st_f f JOIN st_d1 d ON d.id = f.d1 GROUP BY 1, 2 $q$);
SELECT * FROM check_par('left, a NULL argument', $q$ SELECT d.g, count(f.amt), sum(f.amt) FROM st_f f LEFT JOIN st_d1 d ON d.id = f.d1 GROUP BY 1 $q$);
SELECT * FROM check_par('a dimension key twice', $q$ SELECT d.tag, count(*), sum(f.qty) FROM st_f f JOIN st_d2 d ON d.id = f.d2 GROUP BY 1 $q$);
SELECT * FROM check_par('a dimension key twice, left', $q$ SELECT d.tag, count(*) FROM st_f f LEFT JOIN st_d2 d ON d.id = f.d2 GROUP BY 1 $q$);
SELECT * FROM check_par('snowflake', $q$ SELECT p.attr, count(*), sum(f.qty) FROM st_f f JOIN st_d1 d ON d.id = f.d1 JOIN st_p p ON p.id = d.p GROUP BY 1 $q$);
SELECT * FROM check_par('snowflake, left', $q$ SELECT p.gp, count(*) FROM st_f f LEFT JOIN st_d1 d ON d.id = f.d1 LEFT JOIN st_p p ON p.id = d.p GROUP BY 1 $q$);

-- ---- arrays ----
CREATE TABLE un_t (id int, grp int, qty int, label text, tags text[], nums int[]) WITH (autovacuum_enabled = false);
INSERT INTO un_t SELECT i, i % 50, i % 7, CASE WHEN i % 11 = 0 THEN NULL ELSE 'label-' || i % 13 END,
	CASE WHEN i % 17 = 0 THEN NULL WHEN i % 19 = 0 THEN '{}'::text[] WHEN i % 4999 = 0 THEN ARRAY(SELECT 't' || g FROM generate_series(1, 300) g)
		ELSE ARRAY[CASE WHEN i % 23 = 0 THEN NULL ELSE 'tag' || i % 20 END, 'a-much-longer-tag-value-' || i % 37, 'x' || i % 5] END,
	CASE WHEN i % 13 = 0 THEN NULL WHEN i % 29 = 0 THEN '{}'::int[] WHEN i % 4001 = 0 THEN ARRAY(SELECT g FROM generate_series(1, 400) g)
		ELSE ARRAY[i % 10, CASE WHEN i % 31 = 0 THEN NULL ELSE i % 100 END, i % 3] END
-- 120000 rows are 15 granules of 8192 for the 4 threads to expand
FROM generate_series(1, 120000) i;
CREATE INDEX un_t_col ON un_t USING sift_col (id, grp, qty, label, tags, nums) WITH (paths = 'tags[] text, nums[] int4', rollup_columns = 'off');
VACUUM (FREEZE, ANALYZE) un_t;
SELECT * FROM check_par('text elements', $q$ SELECT t, count(*) FROM un_t f CROSS JOIN LATERAL unnest(f.tags) AS u(t) GROUP BY 1 $q$);
SELECT * FROM check_par('text elements, left', $q$ SELECT t, count(*) FROM un_t f LEFT JOIN LATERAL unnest(f.tags) AS u(t) ON true GROUP BY 1 $q$);
SELECT * FROM check_par('text elements with a sum', $q$ SELECT t, count(*), sum(f.qty) FROM un_t f CROSS JOIN LATERAL unnest(f.tags) AS u(t) GROUP BY 1 $q$);
SELECT * FROM check_par('integer elements', $q$ SELECT n, count(*), sum(f.qty) FROM un_t f CROSS JOIN LATERAL unnest(f.nums) AS u(n) GROUP BY 1 $q$);
SELECT * FROM check_par('integer elements, left, counted', $q$ SELECT n, count(*), count(n) FROM un_t f LEFT JOIN LATERAL unnest(f.nums) AS u(n) ON true GROUP BY 1 $q$);
SELECT * FROM check_par('text key of the row', $q$ SELECT f.label, count(*) FROM un_t f CROSS JOIN LATERAL unnest(f.tags) AS u(t) GROUP BY 1 $q$);
SELECT * FROM check_par('a NULL in a counted column', $q$ SELECT n, count(*), count(f.label) FROM un_t f CROSS JOIN LATERAL unnest(f.nums) AS u(n) GROUP BY 1 $q$);
SELECT * FROM check_par('with a predicate', $q$ SELECT t, count(*) FROM un_t f LEFT JOIN LATERAL unnest(f.tags) AS u(t) ON true WHERE f.qty > 2 GROUP BY 1 $q$);
-- ---- quals the scan keys do not say: a <> on a sift_table and a column against a column on an index run over the batch, so the threads leave them to the serial scan ----
CREATE TABLE bq_t (id int, g smallint, v int, w int) USING sift_table;
-- 300000 rows are 37 granules of 8192: many more than the 8 that 4 threads take
INSERT INTO bq_t SELECT i, i % 7, i % 11, i % 5 FROM generate_series(1, 300000) i;
CREATE TABLE bq_c (id int, g smallint, v int, w int);
INSERT INTO bq_c SELECT * FROM bq_t;
CREATE INDEX bq_c_col ON bq_c USING sift_col (g, v, w, id);
VACUUM (FREEZE, ANALYZE) bq_t;
VACUUM (FREEZE, ANALYZE) bq_c;
SELECT * FROM check_par('table <>', $q$ SELECT g, count(*) FROM bq_t WHERE g <> 0 GROUP BY 1 $q$);
SELECT * FROM check_par('table column against column', $q$ SELECT g, count(*) FROM bq_t WHERE v > w GROUP BY 1 $q$);
SELECT * FROM check_par('index column against column', $q$ SELECT g, count(*) FROM bq_c WHERE v > w GROUP BY 1 $q$);
-- ---- the text of a sift_table is the datum of its cell: the empty string, short and long strings, a dictionary of the granule or none ----
CREATE TABLE tx_t (id int, u bigint, s text, d text) USING sift_table;
-- 300000 rows are 37 granules of 8192: many more than the 8 that 4 threads take
INSERT INTO tx_t SELECT i, i % 5, CASE WHEN i % 3 = 0 THEN '' WHEN i % 3 = 1 THEN 'abc' || (i % 4) ELSE repeat('x', 20 + i % 50) END, 'd' || (i % 9) FROM generate_series(1, 300000) i;
VACUUM (FREEZE, ANALYZE) tx_t;
SELECT * FROM check_par('table text key', $q$ SELECT s, count(*) FROM tx_t GROUP BY 1 $q$);
SELECT * FROM check_par('table integer and text keys', $q$ SELECT u, s, count(*) FROM tx_t GROUP BY 1, 2 $q$);
SELECT * FROM check_par('table distinct count by text', $q$ SELECT d, count(DISTINCT s) FROM tx_t GROUP BY 1 $q$);
-- ---- a key held as interned values (a numeric, an extract of a timestamp) is folded by the scan, not by the threads ----
CREATE TABLE gk_c (id int, ts timestamp, n numeric(10, 0), v int);
-- 300000 rows are 37 granules of 8192: many more than the 8 that 4 threads take
INSERT INTO gk_c SELECT i, timestamp '2024-01-01' + i * interval '37 seconds', i % 40, i % 7 FROM generate_series(1, 300000) i;
CREATE INDEX gk_c_col ON gk_c USING sift_col (id, ts, n, v);
VACUUM (FREEZE, ANALYZE) gk_c;
SELECT * FROM check_par('extract key', $q$ SELECT v, extract(minute FROM ts) m, count(*) FROM gk_c GROUP BY 1, 2 $q$);
SELECT * FROM check_par('numeric key', $q$ SELECT v, n, count(*) FROM gk_c GROUP BY 1, 2 $q$);
-- ---- rows of single-row commits wait in the micro-slabs of a sift_table: the leader of a plan with workers writes them out before it lists the units ----
CREATE TABLE sl_t (id int8 PRIMARY KEY, grp int, v int8) USING sift_table;
INSERT INTO sl_t SELECT i, i % 16, i FROM generate_series(1, 8192) i;
CREATE PROCEDURE sl_rows(n int) LANGUAGE plpgsql AS $$
BEGIN
	FOR i IN 1..n LOOP
		INSERT INTO sl_t VALUES (100000 + i, i % 16, 1);
		COMMIT;
	END LOOP;
END $$;
CALL sl_rows(20);
SET parallel_setup_cost = 0;
SET parallel_tuple_cost = 0;
SET min_parallel_table_scan_size = 0;
SELECT * FROM check_par('table micro-slabs', $q$ SELECT grp, count(*), sum(v) FROM sl_t GROUP BY 1 $q$);
RESET parallel_setup_cost;
RESET parallel_tuple_cost;
RESET min_parallel_table_scan_size;
DROP PROCEDURE sl_rows(int);
DROP TABLE sl_t;
DROP FUNCTION check_par(text, text), node_of(text), outcome(text, bool), runs_in_scan(text), pushed(text), runq(text, bool);
DROP TABLE ts_t, st_f, st_d1, st_p, st_d2, un_t, bq_t, bq_c, tx_t, gk_c;
DROP EXTENSION pg_sift;
