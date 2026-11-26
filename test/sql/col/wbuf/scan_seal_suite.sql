-- The write buffer of a sift_col index. The tests that were one file each, one segment for each, each in a schema of its own with the settings reset between them; every segment runs as its test did.
CREATE EXTENSION pg_sift;
-- Format 1: the write buffer is a Format 1 structure; Format 2.0 ignores write_buffer_limit.
SET sift.test_create_format1 = on;

-- >>> col_wbuf_lookup_rescan
CREATE SCHEMA s1;
GRANT USAGE ON SCHEMA s1 TO PUBLIC;
SET search_path = s1, public;
-- An index scan that is rescanned (the inner side of a nested loop) reads the rows of the write buffers once. The equality on a plain column finds them in their records;
-- a lookup that keeps more than a quarter of the buffers (kind k3 below) has the source load the buffers instead, and the next scan, of a kind that keeps few (k2),
-- reuses that source: it listed the rows a second time.
SET sift.async_maintenance = off;
CREATE TABLE rs (id int, parent_id int, kind text, grp int) WITH (autovacuum_enabled = false);
INSERT INTO rs SELECT i, i % 10, 'k' || (i % 5), i % 12 FROM generate_series(1, 600) i;
CREATE INDEX rs_col ON rs USING sift_col (id, parent_id, kind, grp);
VACUUM (FREEZE, ANALYZE) rs;
-- the buffer holds the new versions of 8 rows: 6 of kind k3, 2 of kind k2
UPDATE rs SET grp = 99 WHERE id IN (3, 8, 13, 18, 23, 28, 2, 7);
SET enable_seqscan = off;
SET enable_bitmapscan = off;
SET enable_hashjoin = off;
SET enable_mergejoin = off;
SET enable_material = off;
SET max_parallel_workers_per_gather = 0;
SELECT k.kind, count(*) AS rows, (SELECT count(*) FROM rs WHERE parent_id IN (3, 8, 2, 7) AND rs.kind = k.kind) AS expected
FROM (VALUES (1, 'k3'), (2, 'k2')) k(o, kind),
     LATERAL (SELECT 1 FROM rs e WHERE e.parent_id IN (3, 8, 2, 7) AND e.kind = k.kind OFFSET 0) e
GROUP BY k.kind ORDER BY k.kind;

DROP TABLE rs;
RESET ALL;
SET sift.test_create_format1 = on;
SET client_min_messages = warning;
DROP SCHEMA s1 CASCADE;
RESET client_min_messages;
-- <<< col_wbuf_lookup_rescan

-- >>> col_wbuf_seal_rows
CREATE SCHEMA s2;
GRANT USAGE ON SCHEMA s2 TO PUBLIC;
SET search_path = s2, public;
-- VACUUM seals an open write buffer that holds a quarter of a unit of rows even when its pages are far under a quarter of write_buffer_limit (a column of one small integer is a few
-- bytes a row): the rows leave the buffer, which every probe reads row by row, for a segment. Fewer rows stay where they are.
CREATE TABLE sr (k int) WITH (autovacuum_enabled = off);
CREATE INDEX sr_idx ON sr USING sift_col (k) WITH (write_buffer_limit = 65536);
CREATE FUNCTION buffered(t regclass) RETURNS bigint LANGUAGE sql AS $$ SELECT coalesce(sum(rows), 0) FROM sift.col_segments(t) WHERE kind <> 'immutable' $$;

INSERT INTO sr SELECT g % 100 FROM generate_series(1, 5000) g;
VACUUM sr;
SELECT buffered('sr_idx') AS few_rows_stay;

INSERT INTO sr SELECT g % 100 FROM generate_series(1, 20000) g;
VACUUM sr;
SELECT buffered('sr_idx') AS many_rows_sealed;
SELECT count(*) FROM sr WHERE k = 7;

DROP TABLE sr;
DROP FUNCTION buffered(regclass);
RESET ALL;
SET sift.test_create_format1 = on;
SET client_min_messages = warning;
DROP SCHEMA s2 CASCADE;
RESET client_min_messages;
-- <<< col_wbuf_seal_rows

-- >>> col_wbuf_swap
CREATE SCHEMA s3;
GRANT USAGE ON SCHEMA s3 TO PUBLIC;
SET search_path = s3, public;
-- The rows a transaction buffers for a sift_col index belong to the storage of the index when they were buffered, not to the index of that name at
-- commit: a tool that swaps the storage of two tables and their indexes in the transaction (pg_repack puts the rebuilt copy under the name of the original
-- table that way) must find them in the index that now has that storage. The swap here is the one of pg_repack: the relfilenodes of the heaps and of the
-- indexes exchanged in the catalog.
SET sift.async_maintenance = off;
SET max_parallel_workers_per_gather = 0;
SET client_min_messages = warning;

CREATE TABLE orig (id int, k int, v numeric(10,2)) WITH (autovacuum_enabled = false);
CREATE TABLE copy (id int, k int, v numeric(10,2)) WITH (autovacuum_enabled = false);
INSERT INTO orig SELECT g, g % 7, g % 97 FROM generate_series(1, 50) g;
CREATE INDEX orig_col ON orig USING sift_col (id, k, v);
CREATE INDEX copy_col ON copy USING sift_col (id, k, v);

-- exchanges the storage of two relations of the same kind (the update of pg_class is the swap; the relcache entries follow at the next command)
CREATE FUNCTION swap_storage(a regclass, b regclass) RETURNS void LANGUAGE plpgsql AS $$
DECLARE fa oid; fb oid;
BEGIN
	SELECT relfilenode INTO fa FROM pg_class WHERE oid = a;
	SELECT relfilenode INTO fb FROM pg_class WHERE oid = b;
	UPDATE pg_class SET relfilenode = fb WHERE oid = a;
	UPDATE pg_class SET relfilenode = fa WHERE oid = b;
END $$;

-- the copy is loaded in the transaction (the first row goes in at once, the others are buffered), then takes the place of the original
SET allow_system_table_mods = on;
BEGIN;
INSERT INTO copy SELECT g, g % 7, g % 97 FROM generate_series(1, 300) g;
INSERT INTO copy SELECT 1000 + g, g % 7, g % 97 FROM generate_series(1, 200) g;
SELECT swap_storage('copy', 'orig');
SELECT swap_storage('copy_col', 'orig_col');
COMMIT;
RESET allow_system_table_mods;

-- orig is the loaded table now (500 rows), with the index of the rows it holds; copy has the 50 old rows
SELECT count(*) AS orig_rows FROM orig;
SELECT count(*) AS copy_rows FROM copy;
SELECT sift.col_verify('orig_col', true);
SELECT sift.col_verify('copy_col', true);
SET sift.enable_agg = on;
SELECT k, count(*), sum(v) FROM orig GROUP BY k ORDER BY k;
SET sift.enable_agg = off;
SELECT k, count(*), sum(v) FROM orig GROUP BY k ORDER BY k;
SET sift.enable_agg = on;
SELECT count(*), sum(v) FROM copy;
SET sift.enable_agg = off;
SELECT count(*), sum(v) FROM copy;

DROP TABLE orig, copy;
DROP FUNCTION swap_storage(regclass, regclass);
RESET ALL;
SET sift.test_create_format1 = on;
SET client_min_messages = warning;
DROP SCHEMA s3 CASCADE;
RESET client_min_messages;
-- <<< col_wbuf_swap

DROP EXTENSION pg_sift;
