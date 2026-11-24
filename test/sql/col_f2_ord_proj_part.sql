-- An ordered scan that stops early reads a few rows of each part instead of its first granule: MIN(k), MAX(k) and ORDER BY k LIMIT n over a sift_col index of format major 2 whose
-- parts overlap (two parts and the L0 slab) read the rows of a window of each granule, small first and doubling, and the addresses of the rows read only. Every query runs twice, as the planner is made
-- to run it (an ordered index scan, no Sort) and on the heap (a sequential scan and a Sort), and the rows must be the same, after inserts, deletes, updates, an abort, a vacuum and a transaction's own changes.
CREATE EXTENSION pg_sift;
SET client_min_messages = warning;
SET timezone = 'UTC';
SET sift.async_maintenance = off;
DO $$ BEGIN IF current_setting('sift.test_create_format1', true) IS NOT NULL THEN PERFORM set_config('sift.test_create_format1', 'off', false); END IF; END $$;

-- k is unique where it is not NULL, so that a LIMIT cuts the same rows in both plans; two parts (a vacuum between the first two inserts) and the slab (the third insert)
CREATE TABLE rp_b_a (id int, k int4, v int4);
CREATE INDEX rp_b_a_k ON rp_b_a USING sift_col (k sift.col_int4_ops) WITH (key_order = 'k');
INSERT INTO rp_b_a SELECT i, CASE WHEN i % 13 = 0 THEN NULL ELSE ((i::int8 * 7919) % 100003)::int END, i % 50 FROM generate_series(1, 12000) i;
VACUUM rp_b_a;
INSERT INTO rp_b_a SELECT i, CASE WHEN i % 13 = 0 THEN NULL ELSE ((i::int8 * 7919) % 100003)::int END, i % 50 FROM generate_series(12001, 24000) i;
VACUUM rp_b_a;
INSERT INTO rp_b_a SELECT i, CASE WHEN i % 13 = 0 THEN NULL ELSE ((i::int8 * 7919) % 100003)::int END, i % 50 FROM generate_series(24001, 30000) i;
ANALYZE rp_b_a;
CREATE TABLE rp_b_d (id int, k int8, v int4);
CREATE INDEX rp_b_d_k ON rp_b_d USING sift_col (k sift.col_int8_ops DESC NULLS LAST) WITH (key_order = 'k DESC NULLS LAST');
INSERT INTO rp_b_d SELECT i, CASE WHEN i % 11 = 0 THEN NULL ELSE (i::int8 * 104729) % 50021 END, i % 7 FROM generate_series(1, 5000) i;
VACUUM (ANALYZE) rp_b_d;

CREATE FUNCTION rp_b_run(q text, ord bool, OUT n bigint, OUT md5 text, OUT scans int, OUT sorts int) LANGUAGE plpgsql AS $$
DECLARE line text;
BEGIN
	PERFORM set_config('sift.enable_agg', 'off', true);
	PERFORM set_config('max_parallel_workers_per_gather', '0', true);
	PERFORM set_config('enable_seqscan', (NOT ord)::text, true);
	PERFORM set_config('enable_indexscan', ord::text, true);
	PERFORM set_config('enable_indexonlyscan', ord::text, true);
	PERFORM set_config('enable_sort', (NOT ord)::text, true);
	PERFORM set_config('enable_bitmapscan', 'false', true);
	scans := 0;
	sorts := 0;
	FOR line IN EXECUTE 'EXPLAIN (COSTS OFF) ' || q LOOP
		IF line ~ 'Index (Only )?Scan (Backward )?using rp_b_._k' THEN scans := scans + 1; END IF;
		IF line ~ '^ *(->  )?(Incremental )?Sort$' THEN sorts := sorts + 1; END IF;
	END LOOP;
	EXECUTE 'SELECT count(*), md5(string_agg(w::text, ''|'')) FROM (' || q || ') w' INTO n, md5;
END $$;

CREATE FUNCTION rp_b_check(q text, OUT rows bigint, OUT agree bool, OUT index_scans int, OUT sorts int) LANGUAGE plpgsql AS $$
DECLARE a record; h record;
BEGIN
	SELECT * INTO a FROM rp_b_run(q, true);
	SELECT * INTO h FROM rp_b_run(q, false);
	rows := h.n; agree := a.n = h.n AND a.md5 IS NOT DISTINCT FROM h.md5; index_scans := a.scans; sorts := a.sorts;
	IF NOT agree THEN
		RAISE EXCEPTION 'the ordered scan gives other rows than the heap: % (% against % rows)', q, a.n, h.n;
	END IF;
END $$;

CREATE TABLE rp_b_q (n int, q text);
INSERT INTO rp_b_q VALUES
 (1, 'SELECT k FROM rp_b_a ORDER BY k LIMIT 10'),
 (2, 'SELECT k FROM rp_b_a ORDER BY k LIMIT 1000'),
 (3, 'SELECT k FROM rp_b_a ORDER BY k LIMIT 5000 OFFSET 500'),
 (4, 'SELECT k FROM rp_b_a ORDER BY k DESC LIMIT 10'),
 (5, 'SELECT k FROM rp_b_a ORDER BY k DESC LIMIT 1000'),
 (6, 'SELECT min(k) FROM rp_b_a'),
 (7, 'SELECT max(k) FROM rp_b_a'),
 (8, 'SELECT k FROM rp_b_a WHERE k > 50000 ORDER BY k LIMIT 10'),
 (9, 'SELECT k FROM rp_b_a WHERE k BETWEEN 100 AND 5000 ORDER BY k LIMIT 700'),
 (10, 'SELECT k FROM rp_b_a WHERE k < 300 ORDER BY k DESC LIMIT 10'),
 (11, 'SELECT k FROM rp_b_a WHERE k > 99990 ORDER BY k'),
 (12, 'SELECT k FROM rp_b_a WHERE k = 7919'),
 (13, 'SELECT id, k FROM rp_b_a WHERE k IS NOT NULL ORDER BY k LIMIT 100'),
 (14, 'SELECT id, k FROM rp_b_a WHERE k IS NOT NULL ORDER BY k DESC LIMIT 100'),
 (15, 'SELECT id, k, v FROM rp_b_a WHERE k > 20000 AND v = 7 ORDER BY k LIMIT 25'),
 (16, 'SELECT k FROM rp_b_a ORDER BY k LIMIT 20000'),
 (17, 'SELECT k FROM rp_b_d ORDER BY k DESC NULLS LAST LIMIT 10'),
 (18, 'SELECT k FROM rp_b_d ORDER BY k DESC NULLS LAST LIMIT 900'),
 (19, 'SELECT k FROM rp_b_d ORDER BY k NULLS FIRST LIMIT 10'),
 (20, 'SELECT min(k) FROM rp_b_d'),
 (21, 'SELECT max(k) FROM rp_b_d'),
 (22, 'SELECT k FROM rp_b_d WHERE k < 40000 ORDER BY k DESC NULLS LAST LIMIT 40');
-- one index scan and no Sort in every plan
SELECT n, q, (rp_b_check(q)).* FROM rp_b_q ORDER BY n;

-- rows deleted, updated and inserted after the parts were written (the deletion vectors, the pending deletes and the slab)
DELETE FROM rp_b_a WHERE id % 10 = 3;
UPDATE rp_b_a SET k = k + 100003 WHERE id % 10 = 4;
UPDATE rp_b_a SET k = NULL WHERE id % 10 = 5;
INSERT INTO rp_b_a SELECT 60000 + i, ((i::int8 * 31337) % 100003)::int + 200000, i % 50 FROM generate_series(1, 1000) i;
DELETE FROM rp_b_d WHERE id % 9 = 2;
INSERT INTO rp_b_d SELECT 5000 + i, (i::int8 * 31) % 50021, i % 7 FROM generate_series(1, 300) i;
SELECT n, q, (rp_b_check(q)).* FROM rp_b_q ORDER BY n;

-- a transaction's own changes, then rolled back
BEGIN;
DELETE FROM rp_b_a WHERE id % 7 = 1;
UPDATE rp_b_a SET k = 400000 - id WHERE id % 7 = 2;
INSERT INTO rp_b_a SELECT 90000 + i, -1000 - i, 1 FROM generate_series(1, 60) i;
SELECT n, q, (rp_b_check(q)).* FROM rp_b_q WHERE n <= 12 ORDER BY n;
ROLLBACK;
SELECT n, q, (rp_b_check(q)).* FROM rp_b_q WHERE n <= 12 ORDER BY n;

-- after a vacuum: the parts are rewritten and the pages are all visible
VACUUM rp_b_a;
VACUUM rp_b_d;
SELECT n, q, (rp_b_check(q)).* FROM rp_b_q ORDER BY n;

DROP FUNCTION rp_b_run(text, bool), rp_b_check(text);
DROP TABLE rp_b_q, rp_b_a, rp_b_d;
DROP EXTENSION pg_sift;
