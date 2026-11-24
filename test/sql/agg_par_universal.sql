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
\ir test/agg_helpers.sql

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

-- ---- a star and a snowflake (a chain of dimensions stays on the plan PostgreSQL chooses) ----
CREATE TABLE st_f (id int, d1 int, d2 int, qty int, amt int) WITH (autovacuum_enabled = false);
CREATE TABLE st_d1 (id int PRIMARY KEY, g int, name text, p int);
CREATE TABLE st_p (id int PRIMARY KEY, attr text, gp int);
CREATE TABLE st_d2 (id int, tag int);
INSERT INTO st_p SELECT i, CASE WHEN i % 7 = 0 THEN NULL ELSE 'parent-attribute-' || i % 6 END, i % 4 FROM generate_series(1, 30) i;
INSERT INTO st_d1 SELECT i, CASE WHEN i % 17 = 0 THEN NULL ELSE i % 40 END, CASE WHEN i % 23 = 0 THEN NULL WHEN i % 3 = 0 THEN 'a-long-name-of-' || i % 50 ELSE 'n' || i % 50 END, 1 + i % 35 FROM generate_series(1, 500) i;
INSERT INTO st_d2 SELECT i, i % 5 FROM generate_series(1, 200) i UNION ALL SELECT i, 100 + i % 3 FROM generate_series(1, 20) i;
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
DROP FUNCTION check_par(text, text), node_of(text), outcome(text, bool), runs_in_scan(text), pushed(text), runq(text, bool);
DROP TABLE ts_t, st_f, st_d1, st_p, st_d2, un_t;
DROP EXTENSION pg_sift;
