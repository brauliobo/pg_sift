-- sift.approx_count_distinct (docs/USAGE.md "Approximate distinct counts"): an estimate of the distinct values of a column from a HyperLogLog sketch of 16,384 registers
-- (relative standard error 0.81%). It is the same number in every plan: the normal one, the parallel one, the scan of the rows and the merge of the sketches that an
-- index with the option hll_columns keeps in its units. count(DISTINCT) is exact whatever the options say, in the
-- same query as the estimate too. An index without the option writes no sketch and is the one it was before.
CREATE EXTENSION pg_sift_test CASCADE;
SET client_min_messages = warning;
SET max_parallel_workers_per_gather = 0;
SET max_parallel_maintenance_workers = 0;
SET statement_timeout = '120s';

-- the bound over many seeds and three cardinalities: the largest relative error of 12 sets of each stays within 4 standard errors
SELECT n, round(max(abs(est - n)::numeric / n), 3) < 0.033 AS within_bound
FROM (SELECT n, seed, (SELECT sift.approx_count_distinct(hashint8extended(g, seed)) FROM generate_series(1, n) g) AS est
      FROM unnest(ARRAY[300, 6000, 120000]) n, generate_series(1, 12) seed) s
GROUP BY n ORDER BY n;

-- NULLs, none, one value, repeated values
SELECT sift.approx_count_distinct(x) AS none FROM (SELECT 1 WHERE false) t(x);
SELECT sift.approx_count_distinct(x) AS only_nulls FROM (VALUES (NULL::int), (NULL)) t(x);
SELECT sift.approx_count_distinct(x) AS one, count(DISTINCT x) AS exact FROM (VALUES (7), (7), (NULL), (7)) t(x);
SELECT sift.approx_count_distinct(x) AS small_is_exact, count(DISTINCT x) AS exact FROM generate_series(1, 2000) x;
SELECT sift.approx_count_distinct(x) FROM (VALUES (point(1,2))) t(x);

-- 60,000 rows in about 30 units, every type the units hash: k small ints with NULLs, b many, t texts, d dates, ts timestamps, tz timestamptz, u uuids, s int2, v varchar
CREATE TABLE ap (id int, k int, b bigint, t text, d date, ts timestamp, tz timestamptz, u uuid, s int2, v varchar(40), pad text) WITH (autovacuum_enabled = off);
ALTER TABLE ap ALTER COLUMN pad SET STORAGE PLAIN;
INSERT INTO ap
SELECT g, CASE WHEN g % 13 = 0 THEN NULL ELSE (hashint4(g) & 2147483647) % 900 END,
       (hashint4(g + 1) & 2147483647) % 30000,
       CASE WHEN g % 17 = 0 THEN NULL ELSE 'text ' || (hashint4(g + 2) & 2147483647) % 4000 END,
       date '2020-01-01' + (hashint4(g + 3) & 2147483647) % 1500,
       timestamp '2024-01-01' + ((hashint4(g + 4) & 2147483647) % 20000) * interval '7 minutes',
       timestamptz '2024-01-01 00:00:00+00' + ((hashint4(g + 5) & 2147483647) % 20000) * interval '3 minutes',
       CASE WHEN g % 19 = 0 THEN NULL ELSE md5((g % 2500)::text)::uuid END,
       ((hashint4(g + 6) & 2147483647) % 300)::int2,
       'v' || (hashint4(g + 7) & 2147483647) % 700, repeat('x', 1000)
FROM generate_series(1, 60000) g;
CREATE TABLE ap_off (LIKE ap) WITH (autovacuum_enabled = off);
ALTER TABLE ap_off ALTER COLUMN pad SET STORAGE PLAIN;
INSERT INTO ap_off SELECT * FROM ap ORDER BY id;
CREATE INDEX ap_i ON ap USING sift_col (id, k, b, t, d, ts, tz, u, s, v, pad) WITH (write_buffer_limit = 4096, superunit = 0, hll_columns = 'k, b, t, d, ts, tz, u, s, v');
CREATE INDEX ap_o ON ap_off USING sift_col (id, k, b, t, d, ts, tz, u, s, v, pad) WITH (write_buffer_limit = 4096, superunit = 0, dict_lookup = 'off');
SELECT 'on' AS idx, array_agg(DISTINCT format) AS minors FROM sift._test_col_formats('ap_i')
UNION ALL SELECT 'off', array_agg(DISTINCT format) FROM sift._test_col_formats('ap_o') ORDER BY 1;
VACUUM ap, ap_off;
SELECT sift.col_verify('ap_i', true), sift.col_verify('ap_o', true);
SELECT count(*) FILTER (WHERE section = 'sketches') > 0 AS has_sketches FROM sift.col_sections('ap_i');
SELECT count(*) FILTER (WHERE section = 'sketches') AS off_has_none FROM sift.col_sections('ap_o');
-- under the setting of 3.2 no segment has the sketch
SET sift.col_format_minor = 14;
CREATE TABLE ap_old (LIKE ap) WITH (autovacuum_enabled = off);
ALTER TABLE ap_old ALTER COLUMN pad SET STORAGE PLAIN;
INSERT INTO ap_old SELECT * FROM ap ORDER BY id;
CREATE INDEX ap_old_i ON ap_old USING sift_col (id, k, b, t, d, ts, tz, u, s, v, pad) WITH (write_buffer_limit = 4096, superunit = 0, hll_columns = 'k, b, t, d, ts, tz, u, s, v');
RESET sift.col_format_minor;
SELECT count(*) FILTER (WHERE section = 'sketches') AS old_has_none, pg_relation_size('ap_old_i') = pg_relation_size('ap_o') AS old_is_off_size FROM sift.col_sections('ap_old_i');

-- the number is the one of the normal plan, from the sketches of the units, from the rows of the units and from workers; count(DISTINCT) beside it is exact
CREATE FUNCTION ap_rows(q text, agg bool, preagg bool) RETURNS text LANGUAGE plpgsql AS $$
DECLARE r text;
BEGIN
  EXECUTE format('SET LOCAL sift.enable_agg = %s', agg);
  EXECUTE format('SET LOCAL sift.agg_preagg = %s', preagg);
  EXECUTE format('SET LOCAL sift.agg_force = %s', agg);
  EXECUTE 'SELECT string_agg(r::text, E''\n'' ORDER BY r::text) FROM (' || q || ') r' INTO r;
  RETURN r;
END $$;
CREATE TABLE ap_q (n int, q text);
INSERT INTO ap_q SELECT row_number() OVER (), format('SELECT sift.approx_count_distinct(%s), count(DISTINCT %s), count(*) FROM %%s', c, c)
FROM unnest(ARRAY['k', 'b', 't', 'd', 'ts', 'tz', 'u', 's', 'v']) c;
INSERT INTO ap_q VALUES
	(10, 'SELECT sift.approx_count_distinct(k), sift.approx_count_distinct(b), sift.approx_count_distinct(t), count(DISTINCT b), sum(b) FROM %s'),
	(11, 'SELECT sift.approx_count_distinct(b) FROM %s WHERE id < 20000'),
	(12, 'SELECT sift.approx_count_distinct(b) FROM %s WHERE id BETWEEN 9000 AND 33000'),
	(13, 'SELECT sift.approx_count_distinct(b) FROM %s WHERE id > 1000000'),
	(14, 'SELECT sift.approx_count_distinct(b) FILTER (WHERE k < 100) FROM %s'),
	(15, 'SELECT k %% 5, sift.approx_count_distinct(b), count(DISTINCT t) FROM %s GROUP BY 1'),
	(16, 'SELECT sift.approx_count_distinct(b + 1) FROM %s'),
	(17, 'SELECT sift.approx_count_distinct(id) FROM %s'),
	(18, 'SELECT sift.approx_count_distinct(DISTINCT k) FROM %s');
SELECT n, ap_rows(format(q, 'ap'), true, true) = ap_rows(format(q, 'ap'), true, false) AS sketches_eq_rows,
          ap_rows(format(q, 'ap'), true, true) = ap_rows(format(q, 'ap'), false, true) AS sketches_eq_plain,
          ap_rows(format(q, 'ap'), true, true) = ap_rows(format(q, 'ap_off'), false, true) AS eq_option_off
FROM ap_q ORDER BY n;

-- the estimate is near the exact count, and count(DISTINCT) is the exact one in every plan
SELECT c, abs(a - e)::numeric / e < 0.03 AS near, e = x AS exact_with_option
FROM (SELECT 'b' AS c, sift.approx_count_distinct(b) AS a, count(DISTINCT b) AS e, (SELECT count(DISTINCT b) FROM ap_off) AS x FROM ap
      UNION ALL SELECT 't', sift.approx_count_distinct(t), count(DISTINCT t), (SELECT count(DISTINCT t) FROM ap_off) FROM ap
      UNION ALL SELECT 'k', sift.approx_count_distinct(k), count(DISTINCT k), (SELECT count(DISTINCT k) FROM ap_off) FROM ap) z ORDER BY c;

-- EXPLAIN says it: the sketches answer the units, the aggregate says it is approximate; an index without the option scans rows
CREATE FUNCTION ap_explain(q text) RETURNS TABLE (line text) LANGUAGE plpgsql AS $$
DECLARE l text;
BEGIN
  SET LOCAL sift.agg_force = on;
  FOR l IN EXECUTE 'EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS OFF) ' || q LOOP
    IF l ~ '^\s+(Units|Approximate Distinct|Rows):' THEN RETURN QUERY SELECT trim(l); END IF;
  END LOOP;
END $$;
SELECT * FROM ap_explain('SELECT sift.approx_count_distinct(b) FROM ap');
SELECT * FROM ap_explain('SELECT sift.approx_count_distinct(b) FROM ap_off');
SELECT * FROM ap_explain('SELECT sift.approx_count_distinct(id) FROM ap');
SELECT slug FROM sift.explain_why('SELECT sift.approx_count_distinct(id) FROM ap') WHERE arm = 'single';

-- deletes, updates and the write buffer: units that are not all-visible are read by their rows, the number does not move
DELETE FROM ap WHERE id % 29 = 0 AND id < 30000;
UPDATE ap SET b = b + 100000 WHERE id % 31 = 0;
INSERT INTO ap SELECT g + 100000, g % 900, g % 77777, 't' || g % 3, date '2021-01-01', timestamp '2024-01-01', timestamptz '2024-01-01+00', NULL, 1, 'v', 'x' FROM generate_series(1, 700) g;
SELECT ap_rows('SELECT sift.approx_count_distinct(b), sift.approx_count_distinct(t), count(DISTINCT b) FROM ap', true, true)
     = ap_rows('SELECT sift.approx_count_distinct(b), sift.approx_count_distinct(t), count(DISTINCT b) FROM ap', false, true) AS dirty_same;
SELECT * FROM ap_explain('SELECT sift.approx_count_distinct(b) FROM ap');
VACUUM ap;
SELECT * FROM ap_explain('SELECT sift.approx_count_distinct(b) FROM ap');
SELECT ap_rows('SELECT sift.approx_count_distinct(b), sift.approx_count_distinct(t), count(DISTINCT b) FROM ap', true, true)
     = ap_rows('SELECT sift.approx_count_distinct(b), sift.approx_count_distinct(t), count(DISTINCT b) FROM ap', false, true) AS vacuumed_same;
SELECT sift.col_verify('ap_i', true);

-- workers merge partial sketches: the same number
SET max_parallel_workers_per_gather = 2;
SET parallel_setup_cost = 0;
SET parallel_tuple_cost = 0;
SET min_parallel_table_scan_size = 0;
SELECT ap_rows('SELECT sift.approx_count_distinct(b), sift.approx_count_distinct(t) FROM ap', true, false)
     = ap_rows('SELECT sift.approx_count_distinct(b), sift.approx_count_distinct(t) FROM ap', false, false) AS workers_same;
RESET max_parallel_workers_per_gather;

-- a leaf that is NULL in nearly every row (exceptions, docs/COL-PREAGG.md 11) beside a sketched one, with no element lists: the sketches follow the exception cubes in the section
CREATE TABLE ap_exc (id int, u int, soft int) WITH (autovacuum_enabled = off);
INSERT INTO ap_exc SELECT g, (hashint4(g) & 2147483647) % 5000, CASE WHEN g % 997 = 0 THEN 1 END FROM generate_series(1, 60000) g;
CREATE INDEX ap_exc_i ON ap_exc USING sift_col (id, u, soft) WITH (hll_columns = 'u');
VACUUM ap_exc;
SELECT sift.col_verify('ap_exc_i', true);
SELECT ap_rows('SELECT sift.approx_count_distinct(u), count(*) FROM ap_exc', true, true) = ap_rows('SELECT sift.approx_count_distinct(u), count(*) FROM ap_exc', false, true) AS exceptions_same;
SELECT * FROM ap_explain('SELECT sift.approx_count_distinct(u) FROM ap_exc');

-- the option checks: a column that cannot be hashed the way PostgreSQL does, a name that is not a column, a key_columns leaf
CREATE TABLE ap_bad (id int, c char(5), nm name, f float8, j jsonb);
CREATE INDEX ap_bad_c ON ap_bad USING sift_col (id, c) WITH (hll_columns = 'c');
CREATE INDEX ap_bad_n ON ap_bad USING sift_col (id, nm) WITH (hll_columns = 'nm');
CREATE INDEX ap_bad_x ON ap_bad USING sift_col (id, f) WITH (hll_columns = 'zz');
CREATE INDEX ap_bad_k ON ap_bad USING sift_col (id, f) WITH (hll_columns = 'id', key_columns = 'id');
CREATE INDEX ap_bad_s ON ap_bad USING sift_col (id, f) WITH (hll_columns = 'id,');
CREATE INDEX ap_ok_f ON ap_bad USING sift_col (id, f) WITH (hll_columns = 'f');
SELECT sift.approx_count_distinct(f) FROM (VALUES (0.0::float8), (-0.0), ('NaN'), ('NaN'), (1)) t(f);

DROP TABLE ap, ap_off, ap_old, ap_q, ap_bad, ap_exc;
DROP FUNCTION ap_rows(text, bool, bool), ap_explain(text);
DROP EXTENSION pg_sift_test, pg_sift CASCADE;
