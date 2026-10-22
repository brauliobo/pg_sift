-- The TIDs of a sift_table carry virtual block numbers, not pages of its file: a table that has outgrown 1 GiB of them (131072 blocks, a
-- second file segment) is read by the BM25 scan without any read of a block past the real end of the file (the scan prefetched the heap
-- block of each match, and the file has no such segment). A testing setting moves the virtual blocks there without writing a gigabyte.
CREATE EXTENSION pg_sift;
SET sift.table_vblock_floor = 140000;
CREATE TABLE vb (id int, body text) USING sift_table;
INSERT INTO vb SELECT g, 'order ' || g || CASE WHEN g % 2 = 0 THEN ' express shipping' ELSE ' standard shipping' END FROM generate_series(1, 3000) g;
CREATE INDEX vb_body ON vb USING sift (body);
SELECT min((ctid::text::point)[0]) > 131072 AS past_one_gib, pg_relation_size('vb') < 131072::bigint * 8192 AS file_is_smaller FROM vb;

CREATE FUNCTION vb_agree(q text) RETURNS text LANGUAGE plpgsql AS $$
DECLARE a text; b text;
BEGIN
	PERFORM set_config('enable_seqscan', 'off', true);
	EXECUTE format('SELECT count(*) || '':'' || sum(id) FROM vb WHERE body @@> %L', q) INTO a;
	PERFORM set_config('enable_seqscan', 'on', true);
	PERFORM set_config('enable_indexscan', 'off', true);
	PERFORM set_config('enable_bitmapscan', 'off', true);
	PERFORM set_config('sift.enable_scan', 'off', true);
	PERFORM set_config('sift.enable_count', 'off', true);
	EXECUTE format('SELECT count(*) || '':'' || sum(id) FROM vb WHERE body @@> %L', q) INTO b;
	RETURN a || CASE WHEN a = b THEN '' ELSE ' MISMATCH ' || b END;
END $$;
SELECT vb_agree('express'), vb_agree('standard'), vb_agree('shipping');
-- the rows come back through the scan that returns tuples, with prefetching on
SET sift.prefetch = on;
EXPLAIN (COSTS OFF) SELECT id, body FROM vb WHERE body @@> 'express' ORDER BY id LIMIT 3;
SELECT id, body FROM vb WHERE body @@> 'express' ORDER BY id LIMIT 3;
VACUUM vb;
SELECT vb_agree('express');
-- sift.table_space tells the virtual blocks left: the floor of 140000 and the table's write buffers are taken from 2^32 - 2
SELECT vblocks_left BETWEEN 4294967294 - 140000 - 100000 AND 4294967294 - 140000 AS vblocks_left FROM sift.table_space('vb');
DROP TABLE vb;
DROP FUNCTION vb_agree;
DROP EXTENSION pg_sift;
