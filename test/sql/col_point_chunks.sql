-- A plain index scan on a list of many keys (a prefix of a text leaf, an IN list) with a point directory of heap addresses looks the keys up a chunk at a time, as the
-- executor asks for rows: a LIMIT reads the addresses of a few keys, not of all. The rows are the heap's over every chunk, the write buffer, dead rows (a segment
-- with some is read by its units), a cursor fetched in pieces, a rescan and keys that share a fingerprint.
CREATE EXTENSION pg_sift_test CASCADE;
SET sift.async_maintenance = off;
SET max_parallel_maintenance_workers = 0;

CREATE TABLE lz (id int, k int, s text, pad text DEFAULT repeat('x', 300));
ALTER TABLE lz ALTER COLUMN pad SET STORAGE PLAIN;
INSERT INTO lz (id, k, s)
SELECT i, (i * 7919) % 3000, CASE WHEN i % 61 = 0 THEN NULL ELSE 'p' || to_char((i * 31) % 12000, 'FM00000') END
FROM generate_series(1, 8000) i;
CREATE INDEX lz_t ON lz USING sift_col (id sift.col_int4_ops, k sift.col_int4_ops, s sift.col_text_ops) WITH (write_buffer_limit = 256, point_tid_columns = 'id, k, s', cube_minmax = false);
SELECT sift.col_promote('lz_t'::regclass) >= 0 AS promoted;
INSERT INTO lz (id, k, s)
SELECT i, (i * 7919) % 3000, CASE WHEN i % 61 = 0 THEN NULL ELSE 'p' || to_char((i * 31) % 12000, 'FM00000') END
FROM generate_series(8001, 16000) i;
SELECT sift.col_promote('lz_t'::regclass) >= 0 AS promoted;
INSERT INTO lz (id, k, s)
SELECT i, (i * 7919) % 3000, CASE WHEN i % 61 = 0 THEN NULL ELSE 'p' || to_char((i * 31) % 12000, 'FM00000') END
FROM generate_series(16001, 24000) i;
SELECT sift.col_promote('lz_t'::regclass) >= 0 AS promoted;
VACUUM (ANALYZE) lz;
SELECT count(*) > 1 AS some_segments FROM sift.col_segments('lz_t'::regclass);

-- the index scan, forced
CREATE FUNCTION lz_ids(w text, lim int DEFAULT NULL) RETURNS bigint[] LANGUAGE plpgsql
	SET sift.enable_col_scan = on SET enable_seqscan = off SET enable_bitmapscan = off SET enable_indexonlyscan = off SET max_parallel_workers_per_gather = 0 AS $$
DECLARE r bigint[];
BEGIN
	EXECUTE format('SELECT coalesce(array_agg(id ORDER BY id), ''{}'') FROM (SELECT id FROM lz WHERE %s %s) z', w, CASE WHEN lim IS NULL THEN '' ELSE 'LIMIT ' || lim END) INTO r;
	RETURN r;
END $$;
CREATE FUNCTION lz_seq(w text) RETURNS bigint[] LANGUAGE plpgsql
	SET enable_indexscan = off SET enable_indexonlyscan = off SET enable_bitmapscan = off SET max_parallel_workers_per_gather = 0 AS $$
DECLARE r bigint[];
BEGIN
	EXECUTE format('SELECT coalesce(array_agg(id ORDER BY id), ''{}'') FROM lz WHERE %s', w) INTO r;
	RETURN r;
END $$;
-- the whole answer equals the heap's; a LIMIT gives that many rows (all of them when fewer match), each one a row of the answer, none twice
CREATE FUNCTION lz_check(w text) RETURNS text LANGUAGE plpgsql AS $$
DECLARE want bigint[] := lz_seq(w); bad text := ''; lim int;
BEGIN
	IF lz_ids(w) <> want THEN bad := bad || ' full'; END IF;
	FOR lim IN SELECT unnest(ARRAY[1, 10, 40, 300, 5000]) LOOP
		IF cardinality(lz_ids(w, lim)) <> least(lim, cardinality(want)) OR NOT lz_ids(w, lim) <@ want
		   OR cardinality(lz_ids(w, lim)) <> (SELECT count(DISTINCT x) FROM unnest(lz_ids(w, lim)) x) THEN
			bad := bad || ' limit ' || lim;
		END IF;
	END LOOP;
	RETURN CASE WHEN bad = '' THEN 'ok' ELSE 'MISMATCH' || bad END;
END $$;
-- the shared buffers an index scan with a LIMIT hits
CREATE FUNCTION lz_hits(w text, lim int) RETURNS bigint LANGUAGE plpgsql
	SET sift.enable_col_scan = on SET enable_seqscan = off SET enable_bitmapscan = off SET enable_indexonlyscan = off SET max_parallel_workers_per_gather = 0 AS $$
DECLARE j json; h bigint;
BEGIN
	EXECUTE format('EXPLAIN (ANALYZE, BUFFERS, COSTS off, TIMING off, FORMAT json) SELECT id FROM lz WHERE %s LIMIT %s', w, lim) INTO j;
	h := (j->0->'Plan'->'Plans'->0->>'Shared Hit Blocks')::bigint;
	RETURN h;
END $$;

SELECT lz_check('s LIKE ''p1%'' COLLATE "C"') AS prefix_2000_keys;
SELECT lz_check('s LIKE ''p11%'' COLLATE "C"') AS prefix_1000_keys;
SELECT lz_check('s LIKE ''p000%'' COLLATE "C"') AS prefix_100_keys;
SELECT lz_check('s LIKE ''p0000%'' COLLATE "C"') AS prefix_10_keys;
SELECT lz_check('s LIKE ''zz%'' COLLATE "C"') AS prefix_none;
SELECT lz_check('s >= ''p00500'' AND s < ''p00700'' COLLATE "C"') AS range_with_two_tests;
SELECT lz_check('k IN (' || (SELECT string_agg(g::text, ',') FROM generate_series(10, 400, 3) g) || ')') AS in_list_130_keys;
SELECT lz_check('k IN (' || (SELECT string_agg(g::text, ',') FROM generate_series(10, 400, 3) g) || ') AND id > 20000') AS in_list_and_range;
SELECT lz_check('id IN (' || (SELECT string_agg(g::text, ',') FROM generate_series(1, 20000, 97) g) || ')') AS in_list_unique_ids;

-- a LIMIT reads a fraction of the buffers of the whole answer
SELECT lz_hits('s LIKE ''p1%'' COLLATE "C"', 10) * 4 < lz_hits('s LIKE ''p1%'' COLLATE "C"', 100000) AS limit_reads_less_prefix;
SELECT lz_hits('k IN (' || (SELECT string_agg(g::text, ',') FROM generate_series(10, 2500, 2) g) || ')', 10) * 4 < lz_hits('k IN (' || (SELECT string_agg(g::text, ',') FROM generate_series(10, 2500, 2) g) || ')', 100000) AS limit_reads_less_in;

-- the write buffer and the dead rows: the rows of a unit that has some are read from the unit, and those of the buffer follow the chunks
INSERT INTO lz (id, k, s) SELECT 100000 + i, i % 3000, 'p1' || to_char(i, 'FM0000') FROM generate_series(1, 150) i;
SELECT lz_check('s LIKE ''p1%'' COLLATE "C"') AS buffer_rows;
DELETE FROM lz WHERE id % 7 = 0 AND id < 12000;
SELECT lz_check('s LIKE ''p1%'' COLLATE "C"') AS dead_rows;
UPDATE lz SET s = 'p1' || to_char(id % 90, 'FM000') WHERE id % 11 = 0 AND id BETWEEN 12000 AND 18000;
SELECT lz_check('s LIKE ''p1%'' COLLATE "C"') AS updated_rows;
BEGIN;
INSERT INTO lz (id, k, s) SELECT 200000 + i, i, 'p1x' || i FROM generate_series(1, 90) i;
SELECT lz_check('s LIKE ''p1%'' COLLATE "C"') AS own_uncommitted_rows;
ROLLBACK;
SELECT lz_check('s LIKE ''p1%'' COLLATE "C"') AS after_rollback;
VACUUM lz;
SELECT lz_check('s LIKE ''p1%'' COLLATE "C"') AS after_vacuum;
SELECT sift.col_merge('lz_t'::regclass, true) >= 0 AS merged;
SELECT lz_check('s LIKE ''p1%'' COLLATE "C"') AS after_merge;
SELECT lz_check('k IN (' || (SELECT string_agg(g::text, ',') FROM generate_series(10, 400, 3) g) || ')') AS in_list_after_merge;

-- a cursor fetched in pieces and a rescan
CREATE FUNCTION lz_cursor(w text) RETURNS text LANGUAGE plpgsql
	SET sift.enable_col_scan = on SET enable_seqscan = off SET enable_bitmapscan = off SET enable_indexonlyscan = off SET max_parallel_workers_per_gather = 0 AS $$
DECLARE c refcursor; got bigint[] := '{}'; x bigint;
BEGIN
	OPEN c NO SCROLL FOR EXECUTE format('SELECT id FROM lz WHERE %s', w);
	FOR i IN 1..7 LOOP FETCH c INTO x; EXIT WHEN NOT FOUND; got := got || x; END LOOP;
	FOR i IN 1..33 LOOP FETCH c INTO x; EXIT WHEN NOT FOUND; got := got || x; END LOOP;
	LOOP FETCH c INTO x; EXIT WHEN NOT FOUND; got := got || x; END LOOP;
	CLOSE c;
	RETURN CASE WHEN (SELECT array_agg(v ORDER BY v) FROM unnest(got) v) = lz_seq(w) AND cardinality(got) = (SELECT count(DISTINCT v) FROM unnest(got) v) THEN 'ok' ELSE 'MISMATCH' END;
END $$;
SELECT lz_cursor('s LIKE ''p1%'' COLLATE "C"') AS cursor_prefix;
SELECT lz_cursor('k IN (' || (SELECT string_agg(g::text, ',') FROM generate_series(10, 400, 3) g) || ')') AS cursor_in_list;

SET sift.enable_col_scan = on; SET enable_seqscan = off; SET enable_bitmapscan = off; SET max_parallel_workers_per_gather = 0; SET enable_hashjoin = off; SET enable_mergejoin = off;
SELECT g.p, (SELECT count(*) FROM (SELECT id FROM lz WHERE s LIKE g.p COLLATE "C" LIMIT 25) z) AS got,
	   least(25, (SELECT count(*) FROM lz WHERE s LIKE g.p COLLATE "C")) AS want
FROM (VALUES ('p1%'), ('p2%'), ('p00%'), ('p119%'), ('q%')) g(p);
SELECT count(*) = count(DISTINCT l.id) AS rescan_rows_distinct, count(*) AS rescan_rows
FROM (VALUES ('p10%'), ('p11%'), ('p12%')) g(p), LATERAL (SELECT id FROM lz WHERE s LIKE g.p COLLATE "C") l;
RESET ALL;

-- two keys of one list whose fingerprints and places collide (16 bit fingerprint, 128 slots): the first chunk's lookup of 2077 lists the rows of 4082 too, and the
-- second chunk's lookup of 4082 lists the rows of both again, which the executor must not get twice
CREATE TABLE lzc (id int, k int, pad text DEFAULT repeat('x', 300));
ALTER TABLE lzc ALTER COLUMN pad SET STORAGE PLAIN;
INSERT INTO lzc (id, k) SELECT ks.k % 1000 * 100 + i, ks.k FROM (SELECT unnest(ARRAY[2077, 4082] || ARRAY(SELECT generate_series(2100, 2167))) AS k) ks, generate_series(1, 20) i;
CREATE INDEX lzc_t ON lzc USING sift_col (id sift.col_int4_ops, k sift.col_int4_ops) WITH (write_buffer_limit = 256, point_tid_columns = 'k', cube_minmax = false);
SELECT sift.col_promote('lzc_t'::regclass) >= 0 AS promoted;
CREATE FUNCTION lzc_ids(lim int) RETURNS bigint[] LANGUAGE plpgsql
	SET sift.enable_col_scan = on SET enable_seqscan = off SET enable_bitmapscan = off SET enable_indexonlyscan = off SET max_parallel_workers_per_gather = 0 AS $$
DECLARE r bigint[];
BEGIN
	EXECUTE format('SELECT coalesce(array_agg(id ORDER BY id), ''{}'') FROM (SELECT id FROM lzc WHERE k IN (2077, 4082, %s) LIMIT %s) z', (SELECT string_agg(g::text, ',') FROM generate_series(2100, 2167) g), lim) INTO r;
	RETURN r;
END $$;
SELECT lzc_ids(100000) = (SELECT array_agg(id::bigint ORDER BY id) FROM lzc) AS colliding_keys_once, cardinality(lzc_ids(100000)) AS rows;

-- the verifier agrees with the index after all of it
SELECT sift.col_verify('lz_t'::regclass, true);

DROP TABLE lz, lzc CASCADE;
DROP FUNCTION lz_ids(text, int), lz_seq(text), lz_check(text), lz_hits(text, int), lz_cursor(text), lzc_ids(int);
DROP EXTENSION pg_sift_test, pg_sift CASCADE;
