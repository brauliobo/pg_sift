-- A column whose type is a domain is stored as its base type (the constraint is the table's business): the index builds, aggregates, filters, keys and writes
-- read as the base type's, and every query returns the rows of the plan without the pushdown. A domain over a type the engine does not store, and an array of a domain, keep
-- their own refusal or reading.
CREATE EXTENSION pg_sift;
CREATE EXTENSION pg_sift_helpers;
SET max_parallel_workers_per_gather = 0;
SET seq_page_cost = 1000;
SET random_page_cost = 1000;
SET client_min_messages = warning;

CREATE DOMAIN dom_pos AS int CHECK (VALUE > 0);
CREATE DOMAIN dom_pos2 AS dom_pos;
CREATE DOMAIN dom_txt AS text;
CREATE DOMAIN dom_money AS numeric(10, 2);
CREATE DOMAIN dom_day AS date;
CREATE DOMAIN dom_chr AS char(3);
CREATE DOMAIN dom_pt AS point;

CREATE TABLE dm (id int PRIMARY KEY, a dom_pos, b dom_pos2, t dom_txt, m dom_money, d dom_day, c dom_chr);
INSERT INTO dm SELECT i, 1 + i % 7, 1 + i % 11, 'v' || i % 13, (i % 50) * 1.25, date '2026-01-01' + i % 90, 'c' || i % 4 FROM generate_series(1, 3000) i;
ANALYZE dm;
CREATE INDEX dm_col ON dm USING sift_col (id, a, b, t, m, d, c) WITH (track_changes = 'off');

SELECT a.attname::text, format_type(a.atttypid, a.atttypmod) AS type FROM pg_attribute a WHERE a.attrelid = 'dm_col'::regclass ORDER BY a.attnum;
SELECT s.leaf, s.path, s.kind, s.type FROM sift.col_leaves('dm_col') s ORDER BY s.leaf;

CREATE FUNCTION dm_cases() RETURNS TABLE (q text, pushed boolean, same boolean) LANGUAGE sql AS $$
	SELECT c.q, sift_helpers.plan_mentions_siftagg(c.q), sift_helpers.agg_equals_normal_all(c.q)
	FROM unnest(ARRAY[
		'SELECT a, count(*), sum(id) FROM dm GROUP BY a',
		'SELECT b, count(*), min(id), max(id) FROM dm WHERE a > 3 GROUP BY b',
		'SELECT t, count(*), sum(m) FROM dm GROUP BY t',
		'SELECT m, count(*) FROM dm GROUP BY m',
		'SELECT d, count(*), sum(a) FROM dm WHERE d BETWEEN date ''2026-02-01'' AND date ''2026-02-20'' GROUP BY d',
		'SELECT c, count(*) FROM dm WHERE t IN (''v1'', ''v5'') GROUP BY c',
		'SELECT a, sum(m), avg(m) FROM dm WHERE m > 10 AND c = ''c1'' GROUP BY a'
	]) AS c(q) $$;
SELECT * FROM dm_cases();

-- filters through the index scan itself: the rows are those of a sequential scan
CREATE FUNCTION dm_scans() RETURNS TABLE (q text, bitmap boolean, same boolean) LANGUAGE plpgsql AS $$
DECLARE r_idx text; r_seq text; l text; used boolean;
BEGIN
	FOR q IN SELECT unnest(ARRAY[
		'SELECT id FROM dm WHERE a = 3',
		'SELECT id FROM dm WHERE b IN (2, 5, 9) AND a >= 4',
		'SELECT id FROM dm WHERE t = ''v7''',
		'SELECT id FROM dm WHERE m = 12.50 OR m > 60',
		'SELECT id FROM dm WHERE d BETWEEN date ''2026-02-01'' AND date ''2026-02-05''',
		'SELECT id FROM dm WHERE c = ''c2'' AND a < 3'
	]) LOOP
		SET LOCAL enable_seqscan = off; SET LOCAL enable_indexscan = off; SET LOCAL enable_bitmapscan = on;
		used := false;
		FOR l IN EXECUTE 'EXPLAIN (COSTS OFF) ' || q LOOP used := used OR l LIKE '%Bitmap Index Scan on dm_col%'; END LOOP;
		EXECUTE format('SELECT string_agg(id::text, '','' ORDER BY id) FROM (%s) x', q) INTO r_idx;
		SET LOCAL enable_seqscan = on; SET LOCAL enable_indexscan = off; SET LOCAL enable_bitmapscan = off;
		EXECUTE format('SELECT string_agg(id::text, '','' ORDER BY id) FROM (%s) x', q) INTO r_seq;
		bitmap := used; same := r_idx IS NOT DISTINCT FROM r_seq AND r_seq IS NOT NULL;
		RETURN NEXT;
	END LOOP;
END $$;
SELECT * FROM dm_scans();

-- the index is written: inserts, an update of a domain column, a delete, the maintenance, a rebuild
INSERT INTO dm SELECT 3000 + i, 1 + i % 7, 1 + i % 11, 'v' || i % 13, i, date '2026-05-01', 'c0' FROM generate_series(1, 500) i;
UPDATE dm SET a = 7, m = m + 1 WHERE id % 10 = 0;
DELETE FROM dm WHERE id % 17 = 0;
SELECT * FROM dm_cases();
SELECT * FROM dm_scans();
SELECT sift.col_promote('dm_col') >= 0 AS promoted;
REINDEX INDEX dm_col;
SELECT * FROM dm_cases();
SELECT * FROM dm_scans();

-- the constraint stays the table's: a value it refuses never reaches the index
INSERT INTO dm (id, a) VALUES (99999, 0);

-- a domain over a type with no leaf of its own (held as an image), and an array of a domain
CREATE TABLE dm_pt (id int, p dom_pt);
CREATE INDEX dm_pt_col ON dm_pt USING sift_col (id, p);
CREATE TABLE dm_arr (id int, arr dom_pos[]);
INSERT INTO dm_arr SELECT i, ARRAY[1 + i % 5, 2] FROM generate_series(1, 200) i;
CREATE INDEX dm_arr_col ON dm_arr USING sift_col (id, arr);
SELECT sift_helpers.agg_equals_normal_all('SELECT arr, count(*) FROM dm_arr GROUP BY arr') AS arrays_same,
       sift_helpers.agg_equals_normal_all('SELECT id FROM dm_arr WHERE arr = ARRAY[3, 2]::dom_pos[]') AS array_filter_same;

DROP TABLE dm, dm_pt, dm_arr;
DROP FUNCTION dm_cases(), dm_scans();
DROP DOMAIN dom_pos2, dom_pos, dom_txt, dom_money, dom_day, dom_chr, dom_pt;
DROP EXTENSION pg_sift_helpers;
DROP EXTENSION pg_sift;
