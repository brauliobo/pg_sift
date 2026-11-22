-- Format major 2 for the shapes of a large fact table: a column that holds a value on a few rows of most granules (a soft-delete stamp) keeps the rows it holds, not a bitmap of all
-- of them, and WHERE col IS NULL / IS NOT NULL read the zones before the granules; the pre-aggregate of a granule less the union of its dead rows and those rows is exact; a JSONB key
-- that rows have, lack or hold as a JSON null is one bit per row, so that jsonb ? 'key' is answered by the index without the heap; the partitions and their part lists are
-- parsed once per backend and a commit of the manifest reloads the partition it changed; an UPDATE that stays in a partition writes its rows and its deletes in one record.
CREATE EXTENSION pg_sift_test CASCADE;
SET sift.test_create_format1 = off;
SET sift.enable_agg = off;
SET client_min_messages = warning;
\set VERBOSITY terse

-- The rows the Bitmap Index Scan of a query returns (the heap is not asked to filter them), and whether the answer of the index is the answer of the heap
CREATE FUNCTION idx_rows(q text) RETURNS bigint LANGUAGE plpgsql AS $$
DECLARE j jsonb;
BEGIN
	SET LOCAL enable_seqscan = off; SET LOCAL enable_indexscan = off; SET LOCAL enable_bitmapscan = on;
	EXECUTE 'EXPLAIN (ANALYZE, FORMAT JSON) ' || q INTO j;
	RETURN (SELECT coalesce(sum(("n"->>'Actual Rows')::numeric * ("n"->>'Actual Loops')::numeric), -1)
			FROM jsonb_path_query(j, 'strict $.** ? (@."Node Type" == "Bitmap Index Scan")') n);
END $$;

CREATE FUNCTION f2_same(q text) RETURNS text LANGUAGE plpgsql AS $$
DECLARE via_index text; via_heap text;
BEGIN
	SET LOCAL enable_seqscan = off; SET LOCAL enable_indexscan = off; SET LOCAL enable_bitmapscan = on;
	EXECUTE format('SELECT coalesce(string_agg(r::text, '','' ORDER BY r::text), '''') FROM (%s) r', q) INTO via_index;
	SET LOCAL enable_seqscan = on; SET LOCAL enable_bitmapscan = off;
	EXECUTE format('SELECT coalesce(string_agg(r::text, '','' ORDER BY r::text), '''') FROM (%s) r', q) INTO via_heap;
	RETURN CASE WHEN via_index = via_heap THEN 'same' ELSE 'DIFFERENT: ' || via_index || ' / ' || via_heap END;
END $$;

-- The shared buffers the planner of a query touched
CREATE FUNCTION plan_buffers(q text) RETURNS int LANGUAGE plpgsql AS $$
DECLARE j jsonb;
BEGIN
	SET LOCAL enable_seqscan = off; SET LOCAL enable_indexscan = off;
	EXECUTE 'EXPLAIN (BUFFERS, FORMAT JSON) ' || q INTO j;
	RETURN coalesce((j->0->'Planning'->>'Shared Hit Blocks')::int, 0) + coalesce((j->0->'Planning'->>'Shared Read Blocks')::int, 0);
END $$;

-- What the scans since the last call did: {addresses exact, addresses the heap tests again, granules the zones ruled out, granules read, granules read for a sparse column's rows}
CREATE FUNCTION scans() RETURNS text LANGUAGE sql AS $$
	SELECT (c)[1] || ' exact, ' || (c)[2] || ' recheck, ' || (c)[3] || ' skipped, ' || (c)[4] || ' read, ' || (c)[5] || ' sparse'
	FROM (SELECT sift._test_f2_scan_counters(true) AS c) s $$;

-- A column with a value on a few rows of a granule keeps the rows that have it: 2 bytes each, and none at all when none has it; the bitmap stays for a column that has a value on
-- more than one row in sixteen
SELECT n, nvalid, sift._test_f2_sparse(n, nvalid) FROM (VALUES (8192, 0), (8192, 1), (8192, 19), (8192, 512), (8192, 513), (8192, 8180)) v(n, nvalid);

-- A key that the rows of a granule all have, none has, or have but for a few, and the document shapes that hold it: the bits kept are the smallest of a list of the rows that have
-- it, a list of those that lack it and a bitmap, or none; the rows read back are the rows that have it
SELECT n, nvalid, njnull, noverflow, nrootkey, presence_only, sift._test_f2_key(n, nvalid, njnull, noverflow, nrootkey, presence_only)
FROM (VALUES (8192, 8192, 0, 0, 0, false), (8192, 0, 0, 0, 0, false), (8192, 8000, 0, 0, 0, false), (8192, 100, 3, 2, 1, false), (8192, 8100, 40, 20, 10, false),
			 (8192, 4000, 3, 2, 1, true), (8192, 3000, 0, 0, 0, false), (8192, 4000, 100, 100, 100, false)) v(n, nvalid, njnull, noverflow, nrootkey, presence_only);

-- IS NULL and IS NOT NULL are answered by the zones: a column no row has a value on gives every row for IS NULL without reading it and no row, no granule read, for IS NOT NULL; a sparse
-- column's IS NOT NULL reads the rows it lists; the answers are those of the heap
CREATE TABLE sp (id int, g int, s text, z text);
INSERT INTO sp SELECT i, i % 7, CASE WHEN i % 400 = 50 THEN 'x' END, NULL FROM generate_series(1, 40000) i;
CREATE INDEX sp_i ON sp USING sift_col (id, g, s, z) WITH (key_order = 'id');
SELECT sift._test_f2_check_keys('sp_i');
SELECT scans() IS NOT NULL AS reset;
SELECT idx_rows('SELECT * FROM sp WHERE z IS NULL') AS z_null;
SELECT scans();
SELECT idx_rows('SELECT * FROM sp WHERE z IS NOT NULL') AS z_not_null;
SELECT scans();
SELECT idx_rows('SELECT * FROM sp WHERE s IS NOT NULL') AS s_not_null, count(*) AS heap FROM sp WHERE s IS NOT NULL;
SELECT scans();
SELECT idx_rows('SELECT * FROM sp WHERE s IS NULL') AS s_null, count(*) AS heap FROM sp WHERE s IS NULL;
SELECT scans();
SELECT f2_same('SELECT id FROM sp WHERE s IS NOT NULL') AS s_not_null, f2_same('SELECT id FROM sp WHERE s IS NULL AND g = 3') AS s_null_g3;
SELECT f2_same('SELECT id FROM sp WHERE z IS NULL AND s IS NOT NULL') AS both;

-- The pre-aggregate of a granule less the union of its dead rows and the rows WHERE col IS NULL leaves out: a row that is both is taken out once. Taking the two lists out one after the
-- other counts it twice (naive_mismatched), and the union is the cube of the rows that are left (mismatched)
CREATE TABLE pe (id int, g int, v int, s text);
INSERT INTO pe SELECT i, i % 7, i, CASE WHEN i % 400 = 50 THEN 'x' END FROM generate_series(1, 40000) i;
CREATE INDEX pe_i ON pe USING sift_col (id, g, v, s) WITH (key_order = 'id', track_changes = 'ctid');
SELECT granules, with_excl, corrected, rescanned, mismatched, naive_mismatched, dead, not_null, overlap FROM sift._test_f2_preagg_excl('pe_i', 1, 2, 3);
DELETE FROM pe WHERE id % 800 IN (50, 7, 450);
SELECT granules, with_excl, corrected > 0 AS corrected, rescanned, mismatched, naive_mismatched > 0 AS naive_wrong, dead, not_null, overlap > 0 AS overlap
FROM sift._test_f2_preagg_excl('pe_i', 1, 2, 3);
SELECT f2_same('SELECT g, count(*), sum(v) FROM pe WHERE s IS NULL GROUP BY g') AS grouped;
SELECT f2_same('SELECT id FROM pe WHERE s IS NULL AND id BETWEEN 1 AND 3000') AS rows_left;

-- jsonb ? 'key' on the leaf of a key: JSON null counts as present, a missing key and SQL NULL do not, a root array or string that holds the key does (as in PostgreSQL), and the index
-- returns exactly the rows the heap does, none of them to be tested again
CREATE TABLE ke (id int, g int, doc jsonb);
INSERT INTO ke VALUES (1, 0, '{"flag_a": true}'), (2, 0, '{"flag_a": null}'), (3, 0, '{}'), (4, 0, '{"flag_a": {"x": 1}}'), (5, 0, '{"flag_a": [1, 2], "b": 2}'),
	(6, 0, '{"b": 1}'), (7, 0, '[]'), (8, 0, '["flag_a"]'), (9, 0, '"flag_a"'), (10, 0, '"other"'), (11, 0, '42'), (12, 0, NULL), (13, 0, 'null'),
	(14, 0, '[{"flag_a": 1}]'), (15, 0, '["a", "flag_a", "z"]'), (16, 0, '{"flag_a": "text"}'), (17, 0, '{"flag_a": 1.5}'), (18, 0, 'true');
CREATE INDEX ke_i ON ke USING sift_col (id, g, doc sift.col_jsonb_ops) WITH (key_order = 'g', paths = 'doc.flag_a bool');
SELECT scans() IS NOT NULL AS reset;
SELECT idx_rows('SELECT id FROM ke WHERE doc ? ''flag_a''') AS index_rows;
SELECT scans();
SELECT count(*) AS heap_rows FROM ke WHERE doc ? 'flag_a';
SELECT f2_same('SELECT id FROM ke WHERE doc ? ''flag_a''') AS same;
SELECT string_agg(id::text, ',' ORDER BY id) AS present FROM ke WHERE doc ? 'flag_a';
SELECT f2_same('SELECT id FROM ke WHERE NOT (doc ? ''flag_a'')') AS not_present;
SELECT f2_same('SELECT id FROM ke WHERE doc ? ''flag_a'' AND id > 5') AS with_id;
SELECT scans() IS NOT NULL AS reset;
SELECT idx_rows('SELECT id FROM ke WHERE doc ? ''b''') AS index_rows;
SELECT scans() AS untracked_key_is_tested_again;
SELECT f2_same('SELECT id FROM ke WHERE doc ? ''b''') AS other_key;
SELECT sift._test_f2_check_keys('ke_i');

-- At the scale of a fact table's shape: a third of 40,000 documents have the key; the index returns their addresses exactly, no address is left for the heap to test
CREATE TABLE kb (id int, g int, doc jsonb);
INSERT INTO kb SELECT i, i % 5, CASE i % 6 WHEN 0 THEN '{"flag_a": true, "m": 1}'::jsonb WHEN 1 THEN '{"flag_a": null}'::jsonb WHEN 2 THEN '{"m": 2}'::jsonb
									WHEN 3 THEN '{}'::jsonb WHEN 4 THEN NULL ELSE '["flag_a"]'::jsonb END FROM generate_series(1, 40000) i;
CREATE INDEX kb_i ON kb USING sift_col (id, g, doc sift.col_jsonb_ops) WITH (key_order = 'id', paths = 'doc.flag_a bool');
SELECT scans() IS NOT NULL AS reset;
SELECT idx_rows('SELECT id FROM kb WHERE doc ? ''flag_a''') AS index_rows;
SELECT scans();
SELECT count(*) AS heap_rows FROM kb WHERE doc ? 'flag_a';
SELECT f2_same('SELECT id, g FROM kb WHERE doc ? ''flag_a'' AND g = 2') AS same;
UPDATE kb SET doc = '{"flag_a": 0}' WHERE id % 97 = 0;
UPDATE kb SET doc = '{"m": 0}' WHERE id % 89 = 0;
DELETE FROM kb WHERE id % 1000 = 5;
SELECT f2_same('SELECT id FROM kb WHERE doc ? ''flag_a''') AS same_after_dml;

-- The partitions and the part lists are parsed once per backend (built: the lists read; served: none); a commit of the manifest reloads the partition it changed and no other
CREATE TABLE pc (v int);
CREATE INDEX pc_idx ON pc USING sift_store_test (v);
SELECT sift._test_f2_init('pc_idx');
SELECT sift._test_f2_add_part('pc_idx', 1, NULL, 100, 0, 1) > 0 AS added;
SELECT sift._test_f2_split('pc_idx', 1, NULL, 1000) > 1 AS split;
SELECT part_id AS tail_id FROM sift._test_f2_partitions('pc_idx') WHERE key_lo = 1000 \gset
SELECT sift._test_f2_split_many('pc_idx', :tail_id, 1000, 40, 1001);
SELECT sift._test_f2_plan_cache(true) IS NOT NULL AS reset;
SELECT partitions, parts, total_rows FROM sift._test_f2_plan_snapshot('pc_idx');
SELECT sift._test_f2_plan_cache(true) AS "served,reloaded,built,private,lists_read";
SELECT partitions, parts, total_rows FROM sift._test_f2_plan_snapshot('pc_idx');
SELECT sift._test_f2_plan_cache(true) AS "served,reloaded,built,private,lists_read";
SELECT sift._test_f2_add_part('pc_idx', 1, NULL, 100, 200, 5000) > 0 AS added;
SELECT partitions, parts, total_rows FROM sift._test_f2_plan_snapshot('pc_idx');
SELECT sift._test_f2_plan_cache(true) AS "served,reloaded,built,private,lists_read";

-- Planning a query of an index touches a handful of shared buffers at most: the version of the manifest comes from the registry in shared memory, not from Block 0
SELECT plan_buffers('SELECT count(*) FROM pe WHERE id BETWEEN 100 AND 200') <= 5 AS first_plan_at_most_5_buffers;
SELECT plan_buffers('SELECT count(*) FROM pe WHERE id BETWEEN 100 AND 200') <= 5 AS warm_plan_at_most_5_buffers;
SELECT plan_buffers('SELECT count(*) FROM kb WHERE doc ? ''flag_a''') <= 5 AS warm_jsonb_plan_at_most_5_buffers;

-- An UPDATE of rows of one partition: the new versions and the deletes of the old ones are in one transaction, one partition lock and one record; the answers are those of the heap whether
-- one row is updated, a few of them are, or a statement rolls back
CREATE TABLE up (id int, g int, v int);
INSERT INTO up SELECT i, i % 7, i FROM generate_series(1, 30000) i;
CREATE INDEX up_i ON up USING sift_col (id, g, v) WITH (key_order = 'id', track_changes = 'ctid');
UPDATE up SET v = v + 1 WHERE id = 100;
UPDATE up SET v = v + 1 WHERE id = 101;
UPDATE up SET v = v + 1 WHERE id IN (102, 103, 104);
SELECT f2_same('SELECT id, v FROM up WHERE id BETWEEN 90 AND 120') AS few_rows;
BEGIN;
UPDATE up SET v = v + 5 WHERE id = 200;
UPDATE up SET v = v + 5 WHERE id = 200;
UPDATE up SET v = v + 5 WHERE id = 201;
SAVEPOINT s;
UPDATE up SET v = v + 7 WHERE id = 202;
ROLLBACK TO s;
COMMIT;
SELECT f2_same('SELECT id, v FROM up WHERE id BETWEEN 190 AND 210') AS one_transaction;
UPDATE up SET v = v + 1 WHERE id BETWEEN 1000 AND 1099;
SELECT f2_same('SELECT id, v FROM up WHERE id BETWEEN 990 AND 1110') AS bulk_rows;
UPDATE up SET v = v + 1 WHERE id IN (500, 20000, 29000);
SELECT f2_same('SELECT id, v FROM up WHERE id IN (500, 20000, 29000)') AS rows_of_other_granules;
SELECT f2_same('SELECT g, count(*), sum(v) FROM up GROUP BY g') AS grouped;
SELECT idx_rows('SELECT * FROM up WHERE id BETWEEN 1 AND 30000') AS index_rows, count(*) AS heap_rows FROM up;
SELECT sift._test_f2_check_keys('up_i');

DROP TABLE sp, pe, ke, kb, pc, up;
DROP FUNCTION idx_rows(text), f2_same(text), plan_buffers(text), scans();
DROP EXTENSION pg_sift_test;
DROP EXTENSION pg_sift;
