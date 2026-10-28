-- The build groups the global order of a date or timestamp column by an entity column (order_array_group 'auto: g'): an integer of 100 or more distinct values, at most
-- one in 64 rows of the table apiece, scattered over the heap. The segment keeps the plain order of the column as well (the second slot of the leaf, column format minor 31), so
-- WHERE g = x ORDER BY ts LIMIT n reads the entries of x, and a scan that does not pin g reads the plain order as before. 'off' and a column of the user's are theirs.
CREATE EXTENSION pg_sift_test CASCADE;
SET sift.async_maintenance = off;
SET max_parallel_maintenance_workers = 0;
SET max_parallel_workers_per_gather = 0;
\set VERBOSITY terse

CREATE TABLE ag (id int, g int, v int, ts timestamp, pad text);
ALTER TABLE ag ALTER COLUMN pad SET STORAGE PLAIN;
INSERT INTO ag SELECT i, CASE WHEN i % 97 = 0 THEN NULL ELSE abs(hashint4(i)) % 400 END, abs(hashint4(i + 1)) % 5,
	   CASE WHEN i % 89 = 0 THEN NULL WHEN i % 61 = 0 THEN timestamp '2024-03-01' ELSE timestamp '2024-01-01' + (abs(hashint4(i + 7)) % 20000000) * interval '1 second' END, md5(i::text)
FROM generate_series(1, 120000) i;
-- a group of one row
INSERT INTO ag VALUES (200000, 100000, 1, timestamp '2024-02-02', 'one');
ANALYZE ag;

CREATE INDEX ag_i ON ag USING sift_col (id, g, v, ts, pad);
SELECT reloptions FROM pg_class WHERE relname = 'ag_i';
SELECT min(format) AS format FROM sift.col_segments('ag_i') WHERE kind = 'immutable';
SELECT sift.col_verify('ag_i');

-- an answer is right when the rows are the heap's, in order; 'global order' says the scan read an array
CREATE FUNCTION ag_chk(q text) RETURNS text LANGUAGE plpgsql AS $$
DECLARE pl text; used bool := false; ra text; rb text;
BEGIN
	SET LOCAL sift.enable_agg = on; SET LOCAL sift.enable_col_scan = on; SET LOCAL enable_indexscan = off; SET LOCAL enable_bitmapscan = off; SET LOCAL enable_indexonlyscan = off;
	SET LOCAL sift.agg_force = on; SET LOCAL enable_seqscan = off; SET LOCAL enable_sort = off;
	FOR pl IN EXECUTE 'EXPLAIN (ANALYZE, COSTS off, TIMING off, SUMMARY off) ' || q LOOP used := used OR pl LIKE '%Global Order%'; END LOOP;
	EXECUTE 'SELECT string_agg(x::text, '','' ORDER BY ord) FROM (SELECT row_number() OVER () AS ord, * FROM (' || q || ') r) x' INTO ra;
	SET LOCAL sift.enable_agg = off; SET LOCAL enable_seqscan = on; SET LOCAL enable_sort = on;
	EXECUTE 'SELECT string_agg(x::text, '','' ORDER BY ord) FROM (SELECT row_number() OVER () AS ord, * FROM (' || q || ') r) x' INTO rb;
	RETURN CASE WHEN ra IS DISTINCT FROM rb THEN 'DIFFERS ' || q WHEN used THEN 'global order' ELSE 'units' END;
END $$;

-- the group pinned (the array of x), and not (the plain order), both directions and the pages after a key
SELECT w, result, count(*) FROM (
	SELECT w, ag_chk(format('SELECT id, ts FROM ag WHERE %s ORDER BY ts %s %s, id LIMIT %s', w, d, nl, l)) AS result
	FROM unnest(ARRAY['g = 7', 'g = 7 AND ts >= timestamp ''2024-06-01''', 'g = 0 AND ts < timestamp ''2024-03-01''', 'g = 399', 'g = 100000', 'g = 100001', 'true', 'ts >= timestamp ''2024-09-01''',
					  'ts BETWEEN timestamp ''2024-03-01'' AND timestamp ''2024-03-02''']) w,
		 unnest(ARRAY['ASC', 'DESC']) d, unnest(ARRAY['NULLS FIRST', 'NULLS LAST']) nl, unnest(ARRAY[1, 10, 300]) l) r
GROUP BY 1, 2 ORDER BY 1, 2;
-- a group range or two groups cannot read the array of a group; the plain order has no bound on the key for them
SELECT ag_chk('SELECT id, ts FROM ag WHERE g > 390 ORDER BY ts, id LIMIT 10') AS group_range, ag_chk('SELECT id, ts FROM ag WHERE g = 3 OR g = 4 ORDER BY ts, id LIMIT 10') AS two_groups;

-- the plain order still answers a range of the key from the addresses, with no unit read
SELECT p.rows = 0 AS no_row_read, f.units > p.units AS fewer_units
FROM sift._test_col_counters('ag_i', '3:ge:762382400000000;3:le:762383000000000', 24) p, sift._test_col_counters('ag_i', '3:ge:762382400000000;3:le:762383000000000', 0) f;

-- rows written after the build are in the write buffers and in the answers; a promotion, a merge and a REINDEX keep them
INSERT INTO ag SELECT 300000 + i, i % 400, 1, timestamp '2024-01-01' + (i * 7919 % 20000000) * interval '1 second', 'w' FROM generate_series(1, 3000) i;
UPDATE ag SET ts = timestamp '2023-12-31' WHERE id IN (5, 6, 7) AND g IS NOT NULL;
UPDATE ag SET g = 7 WHERE id IN (8, 9);
DELETE FROM ag WHERE id % 211 = 0;
SELECT ag_chk('SELECT id, ts FROM ag WHERE g = 7 ORDER BY ts, id LIMIT 20') AS buffers_pinned, ag_chk('SELECT id, ts FROM ag ORDER BY ts DESC, id LIMIT 20') AS buffers_plain;
SELECT sift.col_promote('ag_i'::regclass) >= 0 AS promoted;
SELECT sift.col_merge('ag_i'::regclass, true) >= 0 AS merged;
SELECT ag_chk('SELECT id, ts FROM ag WHERE g = 7 ORDER BY ts, id LIMIT 20') AS promoted_pinned, ag_chk('SELECT id, ts FROM ag ORDER BY ts DESC, id LIMIT 20') AS promoted_plain;
SELECT sift.col_verify('ag_i');
REINDEX INDEX ag_i;
SELECT reloptions FROM pg_class WHERE relname = 'ag_i';
SELECT ag_chk('SELECT id, ts FROM ag WHERE g = 7 ORDER BY ts, id LIMIT 20') AS reindexed_pinned, ag_chk('SELECT id, ts FROM ag ORDER BY ts DESC, id LIMIT 20') AS reindexed_plain;

-- the user's: off keeps the plain order only; a column named groups every array and keeps no plain order (a scan that does not pin it reads the units)
ALTER INDEX ag_i SET (order_array_group = 'off');
REINDEX INDEX ag_i;
SELECT reloptions FROM pg_class WHERE relname = 'ag_i';
SELECT ag_chk('SELECT id, ts FROM ag WHERE g = 7 ORDER BY ts, id LIMIT 20') AS off_pinned, ag_chk('SELECT id, ts FROM ag ORDER BY ts DESC, id LIMIT 20') AS off_plain;
ALTER INDEX ag_i SET (order_array_group = 'v', order_array_columns = 'ts', order_columns = 'ts');
REINDEX INDEX ag_i;
SELECT reloptions FROM pg_class WHERE relname = 'ag_i';
SELECT ag_chk('SELECT id, ts FROM ag WHERE v = 2 ORDER BY ts, id LIMIT 20') AS user_pinned, ag_chk('SELECT id, ts FROM ag ORDER BY ts DESC, id LIMIT 20') AS user_plain;
ALTER INDEX ag_i RESET (order_array_group);
ALTER INDEX ag_i RESET (order_array_columns);
ALTER INDEX ag_i RESET (order_columns);
REINDEX INDEX ag_i;
SELECT reloptions FROM pg_class WHERE relname = 'ag_i';

-- the choice is made again by REINDEX: a group of rows too few for a group is dropped (one row in 64 at least)
UPDATE ag SET g = id;
ANALYZE ag;
REINDEX INDEX ag_i;
SELECT reloptions FROM pg_class WHERE relname = 'ag_i';

DROP TABLE ag;
DROP FUNCTION ag_chk(text);
DROP EXTENSION pg_sift_test, pg_sift CASCADE;
