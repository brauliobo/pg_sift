-- The addresses of the rows a segment lost to VACUUM, which a scan of the global order skips an OFFSET with, are listed for the whole segment when it is cut by
-- a key: 8 bytes a dead row. A segment with more dead rows than work_mem takes has none listed (the unit cannot say, and the heap confirms the entries): the rows are the
-- table's under either work_mem, and the list is made only under the larger. (dead_rewrite_pct = 100: VACUUM leaves the dead rows in the segment, it does not rewrite it.)
CREATE EXTENSION pg_sift_test CASCADE;
SET client_min_messages = warning;
SET max_parallel_workers_per_gather = 0;
SET sift.async_maintenance = off;
SET max_parallel_maintenance_workers = 0;

CREATE TABLE t (id int PRIMARY KEY, ent int, d date, pad text DEFAULT repeat('x', 100));
ALTER TABLE t ALTER COLUMN pad SET STORAGE PLAIN;
INSERT INTO t (id, ent, d) SELECT i, 1 + i % 40, date '2024-01-01' + (hashint4(i) & 16777215) % 2000 FROM generate_series(1, 30000) i;
CREATE INDEX t_ix ON t USING sift_col (id, ent, d) WITH (key_cut = 'ent', order_array_columns = 'd', dead_rewrite_pct = 100);
CREATE TABLE p AS TABLE t;
DELETE FROM t WHERE id % 2 = 0;
DELETE FROM p WHERE id % 2 = 0;
VACUUM t;
ANALYZE t;
ANALYZE p;
SET sift.agg_force = on;

CREATE FUNCTION same(q text) RETURNS boolean LANGUAGE plpgsql AS $$
DECLARE a text; b text;
BEGIN
	EXECUTE format('SELECT string_agg(r::text, '','') FROM (%s) r', replace(q, '@', 't')) INTO a;
	EXECUTE format('SELECT string_agg(r::text, '','') FROM (%s) r', replace(q, '@', 'p')) INTO b;
	RETURN a IS NOT DISTINCT FROM b;
END $$;
CREATE FUNCTION all_same() RETURNS boolean LANGUAGE sql AS $$
	SELECT same('SELECT id, d FROM @ ORDER BY d, id LIMIT 20 OFFSET 2000') AND same('SELECT id, d FROM @ ORDER BY d DESC, id LIMIT 20 OFFSET 5000')
	   AND same('SELECT id, d FROM @ ORDER BY d, id LIMIT 20 OFFSET 9000') $$;

-- 15,000 dead rows: 120 kB of addresses. Under 64 kB the segments have no list
CREATE TEMP TABLE seen (step text, rows_same boolean, lists_made int8);
CREATE FUNCTION step(name text) RETURNS void LANGUAGE plpgsql AS $$
DECLARE b0 int8; ok boolean;
BEGIN
	SELECT builds INTO b0 FROM sift._test_col_gdead_stats();
	ok := all_same();
	INSERT INTO seen SELECT name, ok, builds - b0 FROM sift._test_col_gdead_stats();
END $$;
SET work_mem = '64kB';
SELECT step('64 kB');
RESET work_mem;
SELECT step('default');
SELECT step, rows_same, lists_made = 0 AS no_list, lists_made > 0 AS list_made FROM seen ORDER BY step DESC;

DROP FUNCTION step(text);
DROP TABLE seen;
DROP FUNCTION all_same(), same(text);
DROP TABLE t, p;
DROP EXTENSION pg_sift_test;
DROP EXTENSION pg_sift;
