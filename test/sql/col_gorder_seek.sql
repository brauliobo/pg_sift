-- A chunk of an order array may be seekable (column format minor 47): sub-blocks of 32 entries with a fence key each, so that a seek and a scan decode the window they need.
-- The limit gord_seek_pct (115) says how many percent of the plain bytes a seekable chunk may take; 0 writes plain chunks (minor 38), sift.test_col_gorder_seek makes every chunk seekable.
-- The entries, the answers and the verify are the same in both, one table holds segments of both, and a merge writes the current ones.
CREATE EXTENSION pg_sift_test CASCADE;
SET client_min_messages = warning;
SET sift.async_maintenance = off;
SET max_parallel_maintenance_workers = 0;
SET max_parallel_workers_per_gather = 0;

CREATE TABLE gs (id int, k int, u int, pad text DEFAULT repeat('x', 60));
ALTER TABLE gs ALTER COLUMN pad SET STORAGE PLAIN;
-- k: 300 keys of 100 rows, the largest 0, then NULLs (a chunk that ends in NULL rows has fences that do not ascend); u: unique and scattered
INSERT INTO gs (id, k, u) SELECT i, CASE WHEN i % 101 = 0 THEN NULL ELSE -((i * 7919) % 300) END, (i * 104729::bigint) % 1000003 FROM generate_series(1, 30000) i;
VACUUM (ANALYZE) gs;

-- the entries a top-N and a range decode: a plain chunk is decoded from its first entry to the one asked for (the last of the chunk, going down), a seekable one by its windows
SET sift.test_limits = 'gord_seek_pct=0';
CREATE INDEX gs_dec ON gs USING sift_col (id, u) WITH (order_columns = 'u', order_array_columns = 'u', key_columns = 'off', key_cut = 'off', point_tid_columns = 'off');
RESET sift.test_limits;
SELECT sift._test_gorder_decoded(true) >= 0 AS reset;
SELECT count(*) FROM (SELECT id FROM gs ORDER BY u DESC LIMIT 40) z;
CREATE TEMP TABLE dec AS SELECT sift._test_gorder_decoded(true) AS plain_top;
SELECT count(*) FROM (SELECT id FROM gs WHERE u BETWEEN 400000 AND 600000 ORDER BY u LIMIT 20) z;
ALTER TABLE dec ADD COLUMN plain_range int8;
UPDATE dec SET plain_range = sift._test_gorder_decoded(true);
DROP INDEX gs_dec;
SET sift.test_col_gorder_seek = on;
CREATE INDEX gs_dec ON gs USING sift_col (id, u) WITH (order_columns = 'u', order_array_columns = 'u', key_columns = 'off', key_cut = 'off', point_tid_columns = 'off');
RESET sift.test_col_gorder_seek;
SELECT sift._test_gorder_decoded(true) >= 0 AS reset;
SELECT count(*) FROM (SELECT id FROM gs ORDER BY u DESC LIMIT 40) z;
SELECT plain_top > 0 AND sift._test_gorder_decoded(true) * 3 < plain_top AS top_decodes_less FROM dec;
SELECT count(*) FROM (SELECT id FROM gs WHERE u BETWEEN 400000 AND 600000 ORDER BY u LIMIT 20) z;
SELECT plain_range > 0 AND sift._test_gorder_decoded(true) * 3 < plain_range AS range_decodes_less FROM dec;
DROP INDEX gs_dec;

SET sift.test_limits = 'gord_seek_pct=0';
CREATE INDEX gs_old ON gs USING sift_col (id, k, u) WITH (order_columns = 'k, u', order_array_columns = 'k, u', point_columns = 'k', key_columns = 'off', key_cut = 'off');
SET sift.test_col_gorder_seek = on;
CREATE INDEX gs_new ON gs USING sift_col (id, k, u) WITH (order_columns = 'k, u', order_array_columns = 'k, u', point_columns = 'k', key_columns = 'off', key_cut = 'off');
RESET sift.test_limits;
RESET sift.test_col_gorder_seek;
SELECT sift.col_verify('gs_old', true), sift.col_verify('gs_new', true);
SELECT 'gs_old' AS idx, format FROM sift.col_segments('gs_old') WHERE kind = 'immutable' UNION ALL SELECT 'gs_new', format FROM sift.col_segments('gs_new') WHERE kind = 'immutable' ORDER BY 1;

-- the same entries
SELECT leaf, (SELECT count(*) FROM (SELECT pos, key, tid, null_row FROM sift._test_col_gorder('gs_old', leaf) EXCEPT SELECT pos, key, tid, null_row FROM sift._test_col_gorder('gs_new', leaf)) d) AS missing_new,
	   (SELECT count(*) FROM (SELECT pos, key, tid, null_row FROM sift._test_col_gorder('gs_new', leaf) EXCEPT SELECT pos, key, tid, null_row FROM sift._test_col_gorder('gs_old', leaf)) d) AS extra_new,
	   (SELECT count(*) FROM sift._test_col_gorder('gs_new', leaf)) AS entries
FROM generate_series(1, 2) leaf;
-- not much larger than the plain chunks (the writer keeps a seekable chunk where it is within 15%; the setting above lifted that)
SELECT (SELECT sum(bytes) FROM sift.col_sections('gs_new') WHERE section = 'order_arrays') < 1.6 * (SELECT sum(bytes) FROM sift.col_sections('gs_old') WHERE section = 'order_arrays') AS bounded;
DROP INDEX gs_old;

-- the answers through the arrays are the heap's: bounds on a fence, a key that spans fences, offsets that skip, both directions, the NULL rows
CREATE FUNCTION gs_ids(w text, o text, via_sift boolean) RETURNS bigint[] LANGUAGE plpgsql AS $$
DECLARE r bigint[];
BEGIN
	PERFORM set_config('sift.enable_col_scan', via_sift::text, true), set_config('sift.enable_topn', via_sift::text, true), set_config('enable_seqscan', (NOT via_sift)::text, true),
			set_config('enable_indexscan', via_sift::text, true), set_config('enable_bitmapscan', via_sift::text, true), set_config('enable_indexonlyscan', via_sift::text, true);
	EXECUTE format('SELECT coalesce(array_agg(id), ''{}'') FROM (SELECT id FROM gs WHERE %s %s) z', w, o) INTO r;
	RETURN r;
END $$;
CREATE FUNCTION gs_check() RETURNS TABLE (w text, o text, same boolean, n int) LANGUAGE sql AS $$
SELECT w, o, gs_ids(w, o, true) = gs_ids(w, o, false), cardinality(gs_ids(w, o, false))
FROM (VALUES
	('k BETWEEN -100 AND -90', 'ORDER BY id'),
	('k = -7', 'ORDER BY id'),
	('k >= -3', 'ORDER BY id'),
	('k IS NULL', 'ORDER BY id'),
	('u BETWEEN 1000 AND 30000', 'ORDER BY id'),
	('true', 'ORDER BY k, id LIMIT 300'),
	('true', 'ORDER BY k DESC, id LIMIT 300 OFFSET 700'),
	('true', 'ORDER BY k DESC, id LIMIT 20 OFFSET 2100'),
	('k >= -200', 'ORDER BY k, id LIMIT 1000 OFFSET 11'),
	('k <= -50', 'ORDER BY k DESC, id LIMIT 40 OFFSET 3000'),
	('k BETWEEN -120 AND -60', 'ORDER BY k, id LIMIT 60 OFFSET 32'),
	('u >= 500000', 'ORDER BY u, id LIMIT 25'),
	('u >= 500000', 'ORDER BY u DESC, id LIMIT 25 OFFSET 31'),
	('u BETWEEN 100000 AND 100500', 'ORDER BY u, id LIMIT 25'),
	('u < 400000', 'ORDER BY u DESC, id LIMIT 33 OFFSET 2048')
) v(w, o) $$;
SELECT * FROM gs_check();

-- a segment of the plain chunks beside one of the seekable: both answer, a merge writes the current (seekable, as the limit says)
INSERT INTO gs (id, k, u) SELECT 30000 + i, CASE WHEN i % 89 = 0 THEN NULL ELSE -((i * 6841) % 300) END, (i * 7919::bigint) % 1000003 FROM generate_series(1, 20000) i;
SET sift.test_limits = 'gord_seek_pct=0';
SELECT sift.col_promote('gs_new'::regclass) >= 0 AS promoted;
RESET sift.test_limits;
INSERT INTO gs (id, k, u) SELECT 50000 + i, -((i * 6841) % 300), (i * 104729::bigint) % 1000003 FROM generate_series(1, 20000) i;
SET sift.test_col_gorder_seek = on;
SELECT sift.col_promote('gs_new'::regclass) >= 0 AS promoted;
SELECT sift.col_verify('gs_new', true);
SELECT format, count(*) AS segments FROM sift.col_segments('gs_new') WHERE kind = 'immutable' GROUP BY 1 ORDER BY 1;
SELECT bool_and(same) AS all_same, count(*) AS shapes FROM gs_check();
SELECT sift.col_merge('gs_new'::regclass, true) >= 0 AS merged;
SELECT sift.col_verify('gs_new', true);
SELECT format, count(*) AS segments FROM sift.col_segments('gs_new') WHERE kind = 'immutable' GROUP BY 1 ORDER BY 1;
SELECT bool_and(same) AS all_same, count(*) AS shapes FROM gs_check();

-- a delete and an update leave their entries in the arrays: the walk drops them, the OFFSET counts the rows
DELETE FROM gs WHERE id % 7 = 0;
UPDATE gs SET k = -k WHERE id % 11 = 0;
SELECT bool_and(same) AS all_same, count(*) AS shapes FROM gs_check();
VACUUM gs;
SELECT bool_and(same) AS all_same, count(*) AS shapes FROM gs_check();
RESET sift.test_col_gorder_seek;

DROP TABLE gs;
DROP FUNCTION gs_ids(text, text, boolean);
DROP FUNCTION gs_check();
DROP EXTENSION pg_sift_test, pg_sift CASCADE;
