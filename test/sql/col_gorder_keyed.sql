-- A range or an equality on a column with a global order array hands the heap the addresses of its rows (up to 16,384 of them), and those addresses name their units by
-- superblock. In a key-cut segment they do not: the units are cut by the key, not by heap pages. The bitmap scan and the index scan must return the rows of the heap
-- whatever the layout: one segment, a promotion after UPDATE, INSERT and DELETE, VACUUM, REINDEX.
CREATE EXTENSION pg_sift;
SET sift.async_maintenance = off;
SET max_parallel_maintenance_workers = 0;
SET max_parallel_workers_per_gather = 0;
SET client_min_messages = warning;
\set VERBOSITY terse

CREATE TABLE kk (id int, ent int, d date, del timestamp, pad text) WITH (fillfactor = 20, autovacuum_enabled = false);
SELECT setseed(0.5);
INSERT INTO kk SELECT i, 1 + (random() * 39)::int, date '2024-01-01' + (random() * 1000)::int, CASE WHEN random() < 0.05 THEN now() END, repeat('x', 100) FROM generate_series(1, 60000) i;
VACUUM (FREEZE, ANALYZE) kk;

-- the rows of q through the bitmap scan, through the index scan and through the heap; 'used' says the plan took the index
CREATE FUNCTION kk_rows(q text, how text) RETURNS bigint LANGUAGE plpgsql AS $$
DECLARE n bigint := 0; used bool := false; pl text; r record;
BEGIN
	SET LOCAL enable_seqscan = on; SET LOCAL enable_indexscan = on; SET LOCAL enable_indexonlyscan = off; SET LOCAL enable_bitmapscan = on;
	IF how = 'bitmap' THEN SET LOCAL enable_seqscan = off; SET LOCAL enable_indexscan = off;
	ELSIF how = 'index' THEN SET LOCAL enable_seqscan = off; SET LOCAL enable_bitmapscan = off;
	ELSE SET LOCAL enable_indexscan = off; SET LOCAL enable_bitmapscan = off;
	END IF;
	FOR pl IN EXECUTE 'EXPLAIN (COSTS off) ' || q LOOP used := used OR pl LIKE '%kk_i%'; END LOOP;
	IF (how = 'heap') = used THEN RETURN -1; END IF;
	FOR r IN EXECUTE q LOOP n := n + 1; END LOOP;
	RETURN n;
END $$;

CREATE FUNCTION kk_check(layout text) RETURNS TABLE (layout text, cases int, differ int) LANGUAGE sql AS $$
	SELECT layout, count(*)::int, count(*) FILTER (WHERE kk_rows(q, 'bitmap') <> kk_rows(q, 'heap') OR kk_rows(q, 'index') <> kk_rows(q, 'heap'))::int
	FROM (VALUES
		('SELECT id FROM kk WHERE d BETWEEN ''2024-03-01'' AND ''2024-03-04'''),
		('SELECT id FROM kk WHERE d BETWEEN ''2024-03-01'' AND ''2024-03-04'' AND del IS NULL'),
		('SELECT id FROM kk WHERE d BETWEEN ''2024-03-01'' AND ''2024-03-21'' AND del IS NULL'),
		('SELECT id FROM kk WHERE d >= ''2026-08-20'' AND del IS NULL'),
		('SELECT id FROM kk WHERE d < ''2024-01-20'''),
		('SELECT id FROM kk WHERE d = ''2024-03-05'''),
		('SELECT id FROM kk WHERE d = ''2024-03-05'' AND del IS NULL'),
		('SELECT id FROM kk WHERE d BETWEEN ''2024-03-01'' AND ''2024-06-20'' AND ent = 7'),
		('SELECT id FROM kk WHERE d BETWEEN ''2024-03-01'' AND ''2024-06-20'' AND ent IN (3, 7, 9)'),
		('SELECT id FROM kk WHERE d BETWEEN ''2024-03-01'' AND ''2024-06-20'' AND id < 20000'),
		('SELECT id FROM kk WHERE d BETWEEN ''2024-03-01'' AND ''2025-03-01'' AND del IS NULL')
	) v(q) WHERE kk_rows(q, 'heap') > 0
$$;

-- the index is cut by the key, with the array of d
CREATE INDEX kk_i ON kk USING sift_col (id, ent, d, del) WITH (key_cut = 'ent', order_columns = 'd', order_array_columns = 'd');
SELECT sift.col_verify('kk_i');
SELECT * FROM kk_check('one segment');

-- a second segment: rows added, moved and deleted, then promoted
INSERT INTO kk SELECT 60000 + i, 1 + (random() * 39)::int, date '2024-01-01' + (random() * 1000)::int, CASE WHEN random() < 0.05 THEN now() END, repeat('x', 100) FROM generate_series(1, 8000) i;
UPDATE kk SET d = d + 1 WHERE id % 7 = 0;
DELETE FROM kk WHERE id % 11 = 0;
SELECT * FROM kk_check('dirty');
SELECT sift.col_promote('kk_i') >= 0 AS promoted;
SELECT count(*) > 1 AS segments FROM sift.col_segments('kk_i');
SELECT * FROM kk_check('promoted');
VACUUM kk;
SELECT * FROM kk_check('vacuumed');
REINDEX INDEX kk_i;
SELECT * FROM kk_check('reindexed');

-- the units by superblock, where the addresses do name them
ALTER INDEX kk_i SET (key_cut = 'off');
REINDEX INDEX kk_i;
SELECT * FROM kk_check('by superblock');

DROP FUNCTION kk_check(text);
DROP FUNCTION kk_rows(text, text);
DROP TABLE kk;
DROP EXTENSION pg_sift;
