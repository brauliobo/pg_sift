-- A unique key that follows the heap order (a serial id) gets the build's order array, not the point directory: the array is runs of consecutive heap addresses, so it answers the
-- equality in a tenth of the bytes of the directory, for about the same instructions and fewer cold blocks. A key scattered over the heap keeps the directory (its array is as large and reads 2.5 times the instructions),
-- and an index with no array of the build's ('off', a table under 100,000 rows) keeps the directory for the ordered key too. An array the user lists is the user's: whole, and the directory is not added to it.
CREATE EXTENSION pg_sift_test CASCADE;
SET sift.async_maintenance = off;
SET max_parallel_maintenance_workers = 0;
SET max_parallel_workers_per_gather = 0;
\set VERBOSITY terse

CREATE TABLE pk (id int, uid int, ts timestamp, pad text);
INSERT INTO pk SELECT i, (i::bigint * 7919 % 150011)::int, timestamp '2024-01-01' + abs(hashint4(i + 7)) % 5000000 * interval '1 second', md5(i::text)
FROM generate_series(1, 150000) i;
VACUUM (FREEZE, ANALYZE) pk;
CREATE FUNCTION opt(regclass, text) RETURNS text LANGUAGE sql AS $$ SELECT coalesce((SELECT split_part(o, '=', 2) FROM pg_class c, unnest(c.reloptions) o WHERE c.oid = $1 AND o LIKE $2 || '=%'), '-') $$;

CREATE INDEX pk_auto ON pk USING sift_col (id, ts) WITH (key_cut = 'off', key_order = 'off');
-- the same index with the directory of the ordered key and no array of it: what the build chose before
CREATE INDEX pk_dir ON pk USING sift_col (id, ts) WITH (key_cut = 'off', key_order = 'off', point_tid_columns = 'id', order_columns = 'ts', order_array_columns = 'ts');
SELECT i AS idx, opt(i::regclass, 'order_array_columns') AS order_arrays, opt(i::regclass, 'point_tid_columns') AS directory
FROM unnest(ARRAY['pk_auto', 'pk_dir']) i ORDER BY 1;
SELECT pg_relation_size('pk_auto') < pg_relation_size('pk_dir') AS smaller,
	round(pg_relation_size('pk_dir')::numeric / pg_relation_size('pk_auto'), 1) AS dir_over_auto;
-- the lookup of the key reads no unit and no row through the array, as through the directory
SELECT i AS idx, c.units, c.rows_scanned FROM unnest(ARRAY['pk_auto', 'pk_dir']) i, LATERAL sift._test_col_counters(i::regclass, '0:eq:77777', 24) c ORDER BY 1;
DROP INDEX pk_dir;
SELECT sift.col_verify('pk_auto');

SET enable_seqscan = off; SET enable_bitmapscan = off;
EXPLAIN (COSTS OFF) SELECT id, ts FROM pk WHERE id = 77777;
SELECT count(*), min(id), max(ts) FROM pk WHERE id = 77777;
SELECT count(*), min(id) FROM pk WHERE id IN (5, 77777, 149999, 150001);
SET enable_seqscan = on; SET enable_bitmapscan = on;

-- rows written after the build, a promotion and a merge keep the answers
INSERT INTO pk SELECT 150000 + i, i, timestamp '2025-01-01' + i * interval '1 second', 'w' FROM generate_series(1, 3000) i;
DELETE FROM pk WHERE id % 211 = 0;
SELECT sift.col_promote('pk_auto'::regclass) >= 0 AS promoted;
SELECT sift.col_merge('pk_auto'::regclass, true) >= 0 AS merged;
SELECT sift.col_verify('pk_auto');
SET enable_seqscan = off; SET enable_bitmapscan = off;
SELECT count(*), min(id) FROM pk WHERE id IN (5, 77777, 150211, 151000, 152999);
SET enable_seqscan = on; SET enable_bitmapscan = on;

-- a rebuild decides again from the data it finds
REINDEX INDEX pk_auto;
SELECT opt('pk_auto', 'order_array_columns') AS order_arrays, opt('pk_auto', 'point_tid_columns') AS directory;
DROP INDEX pk_auto;

-- a key scattered over the heap keeps the directory (and its own array, as before); no array of the build's, or a table under 100,000 rows: the ordered key keeps the directory; a listed array replaces it
CREATE INDEX pk_uid ON pk USING sift_col (uid, ts) WITH (key_cut = 'off', key_order = 'off');
CREATE INDEX pk_off ON pk USING sift_col (id, ts) WITH (key_cut = 'off', key_order = 'off', order_array_columns = 'off');
CREATE INDEX pk_list ON pk USING sift_col (id, ts) WITH (key_cut = 'off', key_order = 'off', order_columns = 'id', order_array_columns = 'id');
CREATE TABLE pks AS SELECT id, uid, ts, pad FROM pk WHERE id <= 90000;
VACUUM (FREEZE, ANALYZE) pks;
CREATE INDEX pks_i ON pks USING sift_col (id, ts) WITH (key_cut = 'off', key_order = 'off');
SELECT i AS idx, opt(i::regclass, 'order_array_columns') AS order_arrays, opt(i::regclass, 'point_tid_columns') AS directory
FROM unnest(ARRAY['pk_uid', 'pk_off', 'pk_list', 'pks_i']) i ORDER BY 1;

DROP FUNCTION opt(regclass, text);
DROP TABLE pk, pks;
DROP EXTENSION pg_sift_test, pg_sift CASCADE;
