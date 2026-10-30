-- The addresses of a chunk of a global order are coded with the bits of the chunk's largest offset, and as gaps inside a run of equal keys where that is smaller
-- (column format minor 38; sift.test_col_gorder_unpacked writes the 9 bits of the point directory): the entries, the answers and the verify are the same in both,
-- one table holds segments of both, a merge writes the current ones, and a chunk where the addresses of a key do not ascend (the NULL rows follow a key of 0) is written without gaps.
CREATE EXTENSION pg_sift_test CASCADE;
SET client_min_messages = warning;
SET sift.async_maintenance = off;
SET max_parallel_maintenance_workers = 0;
SET max_parallel_workers_per_gather = 0;

CREATE TABLE gg (id int, k int, u int, pad text DEFAULT repeat('x', 60));
ALTER TABLE gg ALTER COLUMN pad SET STORAGE PLAIN;
-- k: 300 keys of 100 rows, the largest 0, then NULLs; u: unique and scattered
INSERT INTO gg (id, k, u) SELECT i, CASE WHEN i % 101 = 0 THEN NULL ELSE -((i * 7919) % 300) END, (i * 104729::bigint) % 1000003 FROM generate_series(1, 30000) i;
VACUUM (ANALYZE) gg;

SET sift.test_col_gorder_unpacked = on;
CREATE INDEX gg_old ON gg USING sift_col (id, k, u) WITH (order_columns = 'k, u', order_array_columns = 'k, u', point_columns = 'k', key_columns = 'off', key_cut = 'off');
RESET sift.test_col_gorder_unpacked;
CREATE INDEX gg_new ON gg USING sift_col (id, k, u) WITH (order_columns = 'k, u', order_array_columns = 'k, u', point_columns = 'k', key_columns = 'off', key_cut = 'off');
SELECT sift.col_verify('gg_old', true), sift.col_verify('gg_new', true);
SELECT 'gg_old' AS idx, format FROM sift.col_segments('gg_old') WHERE kind = 'immutable' UNION ALL SELECT 'gg_new', format FROM sift.col_segments('gg_new') WHERE kind = 'immutable' ORDER BY 1;

-- the same entries, in less space
SELECT leaf, (SELECT count(*) FROM (SELECT pos, key, tid, null_row FROM sift._test_col_gorder('gg_old', leaf) EXCEPT SELECT pos, key, tid, null_row FROM sift._test_col_gorder('gg_new', leaf)) d) AS missing_new,
	   (SELECT count(*) FROM (SELECT pos, key, tid, null_row FROM sift._test_col_gorder('gg_new', leaf) EXCEPT SELECT pos, key, tid, null_row FROM sift._test_col_gorder('gg_old', leaf)) d) AS extra_new,
	   (SELECT count(*) FROM sift._test_col_gorder('gg_new', leaf)) AS entries
FROM generate_series(1, 2) leaf;
SELECT (SELECT sum(bytes) FROM sift.col_sections('gg_new') WHERE section = 'dict_other') < (SELECT sum(bytes) FROM sift.col_sections('gg_old') WHERE section = 'dict_other') AS smaller;
DROP INDEX gg_old;

-- the answers through the arrays are the heap's
CREATE FUNCTION gg_ids(w text, o text, via_sift boolean) RETURNS bigint[] LANGUAGE plpgsql AS $$
DECLARE r bigint[];
BEGIN
	PERFORM set_config('sift.enable_col_scan', via_sift::text, true), set_config('sift.enable_topn', via_sift::text, true), set_config('enable_seqscan', (NOT via_sift)::text, true),
			set_config('enable_indexscan', via_sift::text, true), set_config('enable_bitmapscan', via_sift::text, true), set_config('enable_indexonlyscan', via_sift::text, true);
	EXECUTE format('SELECT coalesce(array_agg(id), ''{}'') FROM (SELECT id FROM gg WHERE %s %s) z', w, o) INTO r;
	RETURN r;
END $$;
CREATE FUNCTION gg_check() RETURNS TABLE (w text, o text, same boolean, n int) LANGUAGE sql AS $$
SELECT w, o, gg_ids(w, o, true) = gg_ids(w, o, false), cardinality(gg_ids(w, o, false))
FROM (VALUES
	('k BETWEEN -100 AND -90', 'ORDER BY id'),
	('k = -7', 'ORDER BY id'),
	('k >= -3', 'ORDER BY id'),
	('k IS NULL', 'ORDER BY id'),
	('u BETWEEN 1000 AND 30000', 'ORDER BY id'),
	('true', 'ORDER BY k, id LIMIT 300'),
	('true', 'ORDER BY k DESC, id LIMIT 300 OFFSET 700'),
	('k >= -200', 'ORDER BY k, id LIMIT 1000 OFFSET 11'),
	('u >= 500000', 'ORDER BY u, id LIMIT 25')
) v(w, o) $$;
SELECT * FROM gg_check();

-- a segment of the earlier layout beside one of the current: both answer, a merge writes one
INSERT INTO gg (id, k, u) SELECT 30000 + i, CASE WHEN i % 89 = 0 THEN NULL ELSE -((i * 6841) % 300) END, (i * 7919::bigint) % 1000003 FROM generate_series(1, 20000) i;
SET sift.test_col_gorder_unpacked = on;
SELECT sift.col_promote('gg_new'::regclass) >= 0 AS promoted;
RESET sift.test_col_gorder_unpacked;
INSERT INTO gg (id, k, u) SELECT 50000 + i, -((i * 6841) % 300), (i * 104729::bigint) % 1000003 FROM generate_series(1, 20000) i;
SELECT sift.col_promote('gg_new'::regclass) >= 0 AS promoted;
SELECT sift.col_verify('gg_new', true);
SELECT format, count(*) AS segments FROM sift.col_segments('gg_new') WHERE kind = 'immutable' GROUP BY 1 ORDER BY 1;
SELECT * FROM gg_check();
SELECT sift.col_merge('gg_new'::regclass, true) >= 0 AS merged;
SELECT sift.col_verify('gg_new', true);
SELECT format, count(*) AS segments FROM sift.col_segments('gg_new') WHERE kind = 'immutable' GROUP BY 1 ORDER BY 1;
SELECT bool_and(same) AS all_same, count(*) AS shapes FROM gg_check();

DROP TABLE gg;
DROP FUNCTION gg_ids(text, text, boolean);
DROP FUNCTION gg_check();
DROP EXTENSION pg_sift_test, pg_sift CASCADE;
