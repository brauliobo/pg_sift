-- SEC_PREAGG version 2 answered by SiftAggV6: the cubes of a part (zero to four group keys, the sums, minima and maxima of integers, numerics and floats, and the sets of
-- distinct values) serve the granules that are clean, and a granule that is not is scanned alone. Every query is run with the node and without it (sift.enable_agg) and the two
-- answers are compared; the first word says they agree, the second that granules of the answer came from the cubes (preagg) or that none did (scan).
CREATE EXTENSION IF NOT EXISTS pg_sift;
SET client_min_messages = warning;
SET max_parallel_workers_per_gather = 0;
SET jit = off;
SET timezone = 'UTC';
DO $$ BEGIN IF current_setting('sift.test_create_format1', true) IS NOT NULL THEN PERFORM set_config('sift.test_create_format1', 'off', false); END IF; END $$;

CREATE FUNCTION answer(q text, node bool) RETURNS text LANGUAGE plpgsql AS $$
DECLARE r text;
BEGIN
	PERFORM set_config('sift.enable_agg', CASE WHEN node THEN 'on' ELSE 'off' END, true);
	EXECUTE 'SELECT coalesce(string_agg(t::text, E''\n'' ORDER BY t::text), '''') FROM (' || q || ') t' INTO r;
	PERFORM set_config('sift.enable_agg', 'on', true);
	RETURN r;
END $$;

CREATE FUNCTION preagg_granules(q text) RETURNS int LANGUAGE plpgsql AS $$
DECLARE l text;
		n int := 0;
BEGIN
	FOR l IN EXECUTE 'EXPLAIN (ANALYZE, COSTS off, TIMING off, SUMMARY off) ' || q LOOP
		n := n + coalesce(substring(l from 'preagg=(\d+)')::int, 0);
	END LOOP;
	RETURN n;
END $$;

CREATE FUNCTION delta_granules(q text) RETURNS int LANGUAGE plpgsql AS $$
DECLARE l text;
		n int := 0;
BEGIN
	FOR l IN EXECUTE 'EXPLAIN (ANALYZE, COSTS off, TIMING off, SUMMARY off) ' || q LOOP
		n := n + coalesce(substring(l from 'preagg_delta=(\d+)')::int, 0);
	END LOOP;
	RETURN n;
END $$;

CREATE FUNCTION parity(q text) RETURNS text LANGUAGE sql AS $$
	SELECT CASE WHEN answer(q, true) = answer(q, false) THEN 'same' ELSE 'DIFF' END $$;

CREATE FUNCTION same(q text) RETURNS text LANGUAGE sql AS $$
	SELECT CASE WHEN answer(q, true) = answer(q, false) THEN 'same' ELSE 'DIFF' END || ' ' || CASE WHEN preagg_granules(q) > 0 THEN 'preagg' ELSE 'scan' END $$;

-- Four columns of few values (the group keys of the cubes), numbers of every width and float, columns of codes and text that makes the granules big enough to hold cubes
CREATE TABLE orders (id int PRIMARY KEY, region text, channel text, tier int2, status text, qty4 int4, qty8 int8, price numeric(12,2), weight float4, score float8, ref int, city text, pad text)
	WITH (autovacuum_enabled = false);
INSERT INTO orders
SELECT i, 'r' || (i % 5), CASE WHEN i % 11 = 0 THEN NULL ELSE 'ch' || (i % 4) END, CASE WHEN i % 13 = 0 THEN NULL ELSE i % 3 END, (ARRAY['new', 'paid', 'shipped'])[1 + i % 3],
	(i * 7919) % 100003, 4000000000000000000 + (i::int8 * 104729) % 1000003 * 1000000000, ((i * 31) % 100000) / 100.0,
	((i % 1000) * 0.25)::float4, (i % 800) * 0.5, i % 40, 'city' || (i % 30),
	encode(sha256((i::text || 'a')::bytea), 'base64') || encode(sha256((i::text || 'b')::bytea), 'base64') || encode(sha256((i::text || 'c')::bytea), 'base64')
-- 100000 rows are 13 granules of 8192: some stay clean and are answered from the cubes while the others are scanned
FROM generate_series(1, 100000) i;
CREATE INDEX orders_col ON orders USING sift_col (id, region, channel, tier, status, qty4, qty8, price, weight, score, ref, city, pad) WITH (key_order = 'id');
VACUUM orders;
ANALYZE orders;

-- No key, one, two, three and four keys; NULL keys (channel, tier); every aggregate of the cubes
SELECT same('SELECT count(*), count(tier), sum(tier), avg(qty4), min(qty8), max(qty8), sum(price), avg(price), min(price), max(price), avg(weight), avg(score), min(score), max(score) FROM orders') AS no_key;
SELECT same('SELECT status, count(*), count(tier), sum(qty4), sum(qty8), min(price), max(price), sum(score) FROM orders GROUP BY status') AS one_key;
SELECT same('SELECT channel, count(*), count(channel), sum(qty4), avg(price) FROM orders GROUP BY channel') AS null_key;
SELECT same('SELECT tier, count(*), sum(qty4), min(weight), max(weight) FROM orders GROUP BY tier') AS null_int_key;
SELECT same('SELECT region, avg(qty4), sum(tier) FROM orders GROUP BY region HAVING count(*) > 1000') AS having_over_cube;

-- count(DISTINCT): integers (lists) and text (dictionary bitmaps, which a part has when it holds the column as a dictionary), grouped, and over the sets of all the granules of the part
SELECT same('SELECT count(DISTINCT ref), count(DISTINCT status) FROM orders') AS distinct_global;
SELECT parity('SELECT count(DISTINCT city), count(DISTINCT channel) FROM orders') AS distinct_text;
SELECT same('SELECT status, count(DISTINCT ref), count(*) FROM orders GROUP BY status') AS distinct_grouped;
SELECT parity('SELECT status, count(DISTINCT city), count(DISTINCT channel) FROM orders GROUP BY status') AS distinct_text_grouped;
SELECT same('SELECT region, count(DISTINCT ref) FROM orders GROUP BY region') AS distinct_by_region;
SELECT parity('SELECT count(DISTINCT qty4) FROM orders') AS distinct_over_the_list_limit;

-- A test on a column that is not a key: the zone of a granule says all of its rows pass (answered from the cubes), some may not (the granule is scanned), none does (it is not read); which granules
-- a part has depends on how the index was built, so the answers that are not from the cubes are only compared
SELECT same('SELECT status, count(*), sum(qty4) FROM orders WHERE id > 0 GROUP BY status') AS zone_all;
SELECT same('SELECT status, count(*), sum(qty4) FROM orders WHERE id >= 1 AND id <= 100000 GROUP BY status') AS zone_all_both_ends;
SELECT same('SELECT status, count(*), sum(qty4) FROM orders WHERE id BETWEEN 20001 AND 80000 GROUP BY status') AS zone_all_and_maybe;
SELECT parity('SELECT region, count(*), sum(qty4) FROM orders WHERE id BETWEEN 20001 AND 80000 AND status = ''paid'' GROUP BY region') AS zone_and_key;
SELECT parity('SELECT status, count(*), sum(price) FROM orders WHERE id > 99000 GROUP BY status') AS zone_maybe_only;
SELECT parity('SELECT status, count(*) FROM orders WHERE id > 100000 GROUP BY status') AS zone_none;
SELECT parity('SELECT count(*), sum(qty4) FROM orders WHERE id < 0') AS zone_none_no_key;
SELECT same('SELECT status, count(*) FROM orders WHERE ref >= 0 GROUP BY status') AS zone_all_on_a_code_column;
SELECT same('SELECT status, count(*) FROM orders WHERE qty4 >= 0 GROUP BY status') AS zone_all_on_a_wide_column;
SELECT parity('SELECT status, count(*) FROM orders WHERE price > 500.50 GROUP BY status') AS value_test_scans;

-- Cubes of two, three and four group keys, with NULL keys, and tests on the keys: equality, lists, ranges, NULL, a prefix, a negation
CREATE TABLE grid (id int PRIMARY KEY, a int2, b text, c int, d text, v int8, pad text) WITH (autovacuum_enabled = false);
INSERT INTO grid
SELECT i, CASE WHEN i % 29 = 0 THEN NULL ELSE i % 4 END, 'b' || ((i / 4) % 3), CASE WHEN i % 31 = 0 THEN NULL ELSE (i / 12) % 3 END, 'd' || ((i / 36) % 2), (i * 37) % 10007,
	encode(sha256((i::text || 'a')::bytea), 'base64') || encode(sha256((i::text || 'b')::bytea), 'base64') || encode(sha256((i::text || 'c')::bytea), 'base64') ||
	encode(sha256((i::text || 'd')::bytea), 'base64') || encode(sha256((i::text || 'e')::bytea), 'base64')
-- 60000 rows are 8 granules of 8192, each with cubes of two to four group keys
FROM generate_series(1, 60000) i;
CREATE INDEX grid_col ON grid USING sift_col (id, a, b, c, d, v, pad) WITH (key_order = 'id');
VACUUM grid;
ANALYZE grid;
SELECT same('SELECT a, count(*), sum(v), min(v), max(v) FROM grid GROUP BY a') AS one_key_null;
SELECT same('SELECT a, b, count(*), sum(v), max(v) FROM grid GROUP BY a, b') AS two_keys;
SELECT same('SELECT a, b, c, count(*), sum(v), min(v) FROM grid GROUP BY a, b, c') AS three_keys;
SELECT same('SELECT a, b, c, d, count(*), count(c), sum(v), avg(v) FROM grid GROUP BY a, b, c, d') AS four_keys;
SELECT same('SELECT d, b, count(*) FROM grid GROUP BY d, b') AS keys_in_other_order;
SELECT same('SELECT b, count(*), sum(v) FROM grid WHERE a = 2 GROUP BY b') AS key_eq;
SELECT same('SELECT b, count(*), sum(v) FROM grid WHERE a IN (1, 3) GROUP BY b') AS key_in;
SELECT same('SELECT b, count(*) FROM grid WHERE c IS NULL GROUP BY b') AS key_is_null;
SELECT same('SELECT a, count(*) FROM grid WHERE c IS NOT NULL AND d <> ''d1'' GROUP BY a') AS key_ne_text;
SELECT same('SELECT a, count(*), min(v) FROM grid WHERE c >= 1 GROUP BY a') AS key_range;
SELECT parity('SELECT a, count(*) FROM grid WHERE b LIKE ''b1%'' GROUP BY a') AS key_prefix;
SELECT same('SELECT a, d, sum(v) FROM grid WHERE a > 0 AND c = 2 AND b = ''b1'' GROUP BY a, d') AS keys_three_tests;
SELECT parity('SELECT count(*), sum(v) FROM grid WHERE a = 9') AS key_matches_nothing;
SELECT same('SELECT b, d, count(*), sum(v) FROM grid WHERE id BETWEEN 10001 AND 50000 AND a = 1 GROUP BY b, d') AS zone_and_keys;
SELECT same('SELECT b, count(DISTINCT c), count(DISTINCT d) FROM grid GROUP BY b') AS distinct_of_keys;

-- The sums of 64-bit integers do not overflow, floats follow PostgreSQL where they are exact, and a numeric keeps its scale
SELECT same('SELECT sum(qty8), avg(qty8), min(qty8), max(qty8) FROM orders') AS sum_past_64_bits;
SELECT sum(qty8) AS sum_qty8 FROM orders;
SELECT same('SELECT sum(price), avg(price), min(price), max(price) FROM orders WHERE region = ''r3''') AS numeric_scale;

-- NaN, +Infinity, -Infinity and -0 in the rows of a group
CREATE TABLE readings (id int PRIMARY KEY, kind int, f4 float4, f8 float8, pad text) WITH (autovacuum_enabled = false);
INSERT INTO readings
SELECT i, i % 7,
	CASE WHEN i % 7 = 1 AND i % 2000 = 8 THEN 'NaN' WHEN i % 7 = 2 AND i % 2000 = 9 THEN 'Infinity' WHEN i % 7 = 3 AND i % 2000 = 10 THEN '-Infinity'
		 WHEN i % 7 = 4 AND i % 2000 IN (11, 1011) THEN '-0' WHEN i % 7 = 6 AND i % 2000 IN (13, 1013) THEN 'NaN' ELSE (i % 1000) * 0.25 END::float4,
	CASE WHEN i % 7 = 1 AND i % 2000 = 8 THEN 'NaN' WHEN i % 7 = 2 AND i % 2000 = 9 THEN 'Infinity' WHEN i % 7 = 3 AND i % 2000 = 10 THEN '-Infinity'
		 WHEN i % 7 = 4 AND i % 2000 IN (11, 1011) THEN '-0' WHEN i % 7 = 5 AND i % 2000 IN (12, 1012) THEN 'Infinity' WHEN i % 7 = 5 AND i % 2000 = 1012 THEN '-Infinity'
		 WHEN i % 7 = 6 AND i % 2000 IN (13, 1013) THEN 'NaN' WHEN i % 7 = 6 AND i % 2000 = 1013 THEN 'Infinity' ELSE (i % 800) * 0.5 END::float8,
	encode(sha256((i::text || 'a')::bytea), 'base64') || encode(sha256((i::text || 'b')::bytea), 'base64') || encode(sha256((i::text || 'c')::bytea), 'base64')
FROM generate_series(1, 40000) i;
UPDATE readings SET f8 = '-0' WHERE kind = 0 AND id <= 14;
CREATE INDEX readings_col ON readings USING sift_col (id, kind, f4, f8, pad) WITH (key_order = 'id');
VACUUM readings;
SELECT kind, sum(f8)::text AS sum_f8, min(f8)::text AS min_f8, max(f8)::text AS max_f8, count(f8) FROM readings WHERE kind <= 6 GROUP BY kind ORDER BY kind;
SELECT same('SELECT kind, count(*), sum(f8), min(f8), max(f8), avg(f8) FROM readings GROUP BY kind') AS float8_specials;
SELECT same('SELECT kind, count(*), avg(f4), min(f4), max(f4) FROM readings GROUP BY kind') AS float4_specials;
SELECT same('SELECT avg(f4), sum(f8), min(f8), max(f8) FROM readings') AS float_specials_global;
SELECT same('SELECT kind, min(f8), max(f8) FROM readings WHERE kind IN (0, 4) GROUP BY kind') AS negative_zero;

-- Parts: later inserts are parts of their own, compaction joins the cubes of parts, and a count(DISTINCT) is over the sets of all of them
CREATE TABLE events (id int PRIMARY KEY, site int, kind text, amount int8, code int, pad text) WITH (autovacuum_enabled = false);
INSERT INTO events SELECT i, i % 6, 'k' || (i % 4), (i * 37) % 10007, i % 30,
-- 60000 rows are 8 granules of 8192: the first part of the table
	encode(sha256((i::text || 'a')::bytea), 'base64') || encode(sha256((i::text || 'b')::bytea), 'base64') || encode(sha256((i::text || 'c')::bytea), 'base64') FROM generate_series(1, 60000) i;
CREATE INDEX events_col ON events USING sift_col (id, site, kind, amount, code, pad) WITH (key_order = 'id');
INSERT INTO events SELECT i, i % 6, 'k' || (i % 4 + 2 * (i % 2)), (i * 37) % 10007 + 5, 20 + i % 30,
-- 60000 more rows (ids 60001 to 120000) as a part of their own, whose cubes the compaction joins with those of the first
	encode(sha256((i::text || 'a')::bytea), 'base64') || encode(sha256((i::text || 'b')::bytea), 'base64') || encode(sha256((i::text || 'c')::bytea), 'base64') FROM generate_series(60001, 120000) i;
VACUUM events;
SELECT same('SELECT site, count(*), sum(amount), min(amount), max(amount) FROM events GROUP BY site') AS parts_grouped;
SELECT same('SELECT count(DISTINCT code), count(DISTINCT kind) FROM events') AS parts_distinct_global;
SELECT same('SELECT site, count(DISTINCT code), count(DISTINCT kind) FROM events GROUP BY site') AS parts_distinct_grouped;
SELECT same('SELECT kind, site, count(*) FROM events WHERE id > 30000 GROUP BY kind, site') AS parts_two_keys;
INSERT INTO events SELECT i, i % 6, 'k9', i % 100, 60 + i % 5,
	encode(sha256((i::text || 'a')::bytea), 'base64') || encode(sha256((i::text || 'b')::bytea), 'base64') || encode(sha256((i::text || 'c')::bytea), 'base64') FROM generate_series(120001, 125000) i;
SELECT same('SELECT kind, count(*), count(DISTINCT code) FROM events GROUP BY kind') AS rows_not_in_a_part_yet;

-- UPDATE and DELETE churn, 1% to 40% of the rows: a granule with a few dead rows is corrected from its cube (preagg), one with more is scanned alone while the clean granules of its part
-- still come from the cubes (mixed), and a transaction sees its own changes and no other's. A query that is answered from as many granules as before the churn says preagg.
CREATE TABLE churn (LIKE orders INCLUDING DEFAULTS) WITH (autovacuum_enabled = false);
INSERT INTO churn SELECT * FROM orders;
ALTER TABLE churn ADD PRIMARY KEY (id);
CREATE INDEX churn_col ON churn USING sift_col (id, region, channel, tier, status, qty4, qty8, price, weight, score, ref, city, pad) WITH (key_order = 'id');
VACUUM churn;
SET sift.agg_force = on;
CREATE TABLE served (label text PRIMARY KEY, n int);
CREATE FUNCTION graded(label text, q text) RETURNS text LANGUAGE plpgsql AS $$
DECLARE n int := preagg_granules(q);
		base int;
BEGIN
	SELECT served.n INTO base FROM served WHERE served.label = graded.label;
	IF NOT FOUND THEN
		INSERT INTO served VALUES (label, n);
		base := n;
	END IF;
	RETURN CASE WHEN answer(q, true) = answer(q, false) THEN 'same' ELSE 'DIFF' END || ' ' || CASE WHEN n = 0 THEN 'scan' WHEN n >= base THEN 'preagg' ELSE 'mixed' END;
END $$;
CREATE FUNCTION churn_checks() RETURNS TABLE (q text, result text) LANGUAGE sql AS $$
	VALUES ('count and sum', graded('count and sum', 'SELECT status, count(*), sum(qty4), sum(price) FROM churn GROUP BY status')),
		   ('min and max', graded('min and max', 'SELECT status, min(qty8), max(score), count(tier) FROM churn GROUP BY status')),
		   ('global', graded('global', 'SELECT count(*), sum(qty8), avg(weight) FROM churn')),
		   ('distinct', graded('distinct', 'SELECT region, count(DISTINCT ref), count(*) FROM churn GROUP BY region')),
		   ('zone and key', graded('zone and key', 'SELECT count(*), sum(qty4) FROM churn WHERE id BETWEEN 10000 AND 90000 AND region <> ''r1''')) $$;
SELECT * FROM churn_checks();
DELETE FROM churn WHERE (id BETWEEN 1001 AND 8000 AND id % 200 = 107) OR (id > 98000 AND id % 2 = 0);
SELECT 'delete 35 rows of one granule, 40% of the last' AS churn, * FROM churn_checks();
VACUUM churn;
SELECT 'vacuum: the 35 dead rows are subtracted' AS churn, * FROM churn_checks();
SELECT delta_granules('SELECT status, count(*), sum(qty4), sum(price) FROM churn GROUP BY status') > 0 AS corrected_from_the_cube,
	   delta_granules('SELECT status, count(*), count(DISTINCT ref) FROM churn GROUP BY status') = 0 AS distinct_is_not_corrected;
UPDATE churn SET qty4 = qty4 + 1, score = score + 0.5 WHERE id BETWEEN 40001 AND 48000 AND id % 5 = 0;
SELECT 'update 20% of a granule' AS churn, * FROM churn_checks();
UPDATE churn SET status = 'paid', region = 'r' || ((substr(region, 2)::int + 1) % 5) WHERE id % 5 = 1 AND id > 60000;
SELECT 'update the keys of 20% of the rows' AS churn, * FROM churn_checks();
DELETE FROM churn WHERE id % 100 = 7;
VACUUM churn;
SELECT 'delete 1% of every granule' AS churn, * FROM churn_checks();
DELETE FROM churn WHERE id % 5 IN (0, 2);
SELECT 'delete 40% of the rows' AS churn, * FROM churn_checks();
VACUUM churn;
SELECT 'vacuum' AS churn, * FROM churn_checks();
UPDATE churn SET qty8 = qty8 - 1 WHERE id BETWEEN 90001 AND 91000;
SELECT 'update after vacuum' AS churn, * FROM churn_checks();
BEGIN;
DELETE FROM churn WHERE id % 10 = 1;
SELECT 'own delete' AS txn, * FROM churn_checks();
UPDATE churn SET qty4 = 0 WHERE id % 10 = 6;
SELECT 'own delete and update' AS txn, * FROM churn_checks();
ROLLBACK;
SELECT 'rolled back' AS txn, * FROM churn_checks();
BEGIN;
INSERT INTO churn SELECT id + 200000, region, channel, tier, status, qty4, qty8, price, weight, score, ref, city, pad FROM churn WHERE id % 1000 = 5;
SELECT 'own insert' AS txn, * FROM churn_checks();
COMMIT;
SELECT 'committed insert' AS txn, * FROM churn_checks();
VACUUM churn;
SELECT 'vacuum again' AS txn, * FROM churn_checks();
RESET sift.agg_force;

-- preagg = 'off' stores no cubes and every granule is scanned; 'auto' stores them
CREATE TABLE cubes_auto (id int PRIMARY KEY, g int, v int, pad text) WITH (autovacuum_enabled = off);
-- 100000 rows are 13 granules of 8192 whose cubes preagg = auto stores and preagg = off does not
INSERT INTO cubes_auto SELECT i, i % 5, i, md5(i::text) || md5((i + 1)::text) || md5((i + 2)::text) FROM generate_series(1, 100000) i;
CREATE TABLE cubes_off (LIKE cubes_auto) WITH (autovacuum_enabled = off);
INSERT INTO cubes_off SELECT * FROM cubes_auto;
ALTER TABLE cubes_off ADD PRIMARY KEY (id);
CREATE INDEX cubes_auto_col ON cubes_auto USING sift_col (id, g, v, pad) WITH (key_order = 'id', preagg = 'auto');
CREATE INDEX cubes_off_col ON cubes_off USING sift_col (id, g, v, pad) WITH (key_order = 'id', preagg = 'off');
VACUUM (FREEZE, ANALYZE) cubes_auto, cubes_off;
SELECT (SELECT sum(preagg_bytes) > 0 FROM sift.col_segments('cubes_auto_col')) AS auto_stores_cubes,
       (SELECT sum(preagg_bytes) = 0 FROM sift.col_segments('cubes_off_col')) AS off_stores_none;
SELECT same('SELECT g, count(*), sum(v) FROM cubes_auto GROUP BY g') AS auto, same('SELECT g, count(*), sum(v) FROM cubes_off GROUP BY g') AS off;

-- a column that is NULL in nearly every row (a soft-delete stamp) with a value of its own in each of the others is a dimension of the cubes: its few rows make few cells, and the NULL group answers IS NULL
CREATE TABLE soft (id int PRIMARY KEY, kind int, val int8, stamp timestamptz, pad text) WITH (autovacuum_enabled = off);
ALTER TABLE soft ALTER COLUMN pad SET STORAGE PLAIN;
INSERT INTO soft SELECT i, i % 6, (i % 997) * 3 - 1000, CASE WHEN i % 60 = 0 THEN timestamptz '2025-06-01 00:00:00+00' + i * interval '1 second' END, (SELECT string_agg(md5(i::text || s::text), '') FROM generate_series(1, 10) s) FROM generate_series(1, 24000) i;
CREATE INDEX soft_col ON soft USING sift_col (id, kind, val, stamp) WITH (key_order = 'id');
VACUUM (FREEZE, ANALYZE, DISABLE_PAGE_SKIPPING) soft;
SELECT parity('SELECT kind, count(*), sum(val) FROM soft WHERE stamp IS NULL GROUP BY kind') AS stamp_is_null,
       parity('SELECT kind, count(*), sum(val) FROM soft WHERE stamp IS NOT NULL GROUP BY kind') AS stamp_is_not_null,
       parity('SELECT count(*), sum(val), min(val), max(val) FROM soft WHERE stamp IS NULL') AS stamp_is_null_global;
UPDATE soft SET stamp = timestamptz '2026-01-01 00:00:00+00' + id * interval '1 second' WHERE id % 1000 = 3;
DELETE FROM soft WHERE id % 500 = 7;
SELECT parity('SELECT kind, count(*), sum(val) FROM soft WHERE stamp IS NULL GROUP BY kind') AS after_churn;
VACUUM soft;
SELECT parity('SELECT kind, count(*), sum(val) FROM soft WHERE stamp IS NULL GROUP BY kind') AS after_vacuum,
       parity('SELECT kind, count(*), sum(val) FROM soft WHERE stamp IS NOT NULL GROUP BY kind') AS not_null_after_vacuum;

DROP TABLE churn, served, events, readings, grid, orders, soft, cubes_auto, cubes_off;
DROP FUNCTION churn_checks(), graded(text, text), same(text), parity(text), delta_granules(text), preagg_granules(text), answer(text, bool);
DROP EXTENSION pg_sift;
