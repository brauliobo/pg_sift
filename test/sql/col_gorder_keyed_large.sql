-- col_gorder_keyed at a size where the table is long: rows past heap block 12,000 (the units there are the ones a wrong address-to-unit mapping loses first), ranges
-- on both sides of the 16,384 rows a range hands the heap as addresses, and several segments (a promotion). Bitmap scan and index scan against the heap.
CREATE EXTENSION pg_sift;
SET sift.async_maintenance = off;
SET max_parallel_maintenance_workers = 0;
SET max_parallel_workers_per_gather = 0;
SET client_min_messages = warning;
\set VERBOSITY terse

CREATE TABLE kl (id int, ent int, d date, ts timestamp, del timestamp, pad text) WITH (fillfactor = 10, autovacuum_enabled = false);
ALTER TABLE kl ALTER COLUMN pad SET STORAGE PLAIN;
SELECT setseed(0.7);
INSERT INTO kl SELECT i, 1 + (random() * 59)::int, date '2022-01-01' + (random() * 1500)::int, timestamp '2022-01-01' + random() * 1500 * interval '1 day',
	CASE WHEN random() < 0.04 THEN now() END, repeat('x', 150) FROM generate_series(1, 105000) i;
VACUUM (FREEZE, ANALYZE) kl;
SELECT pg_relation_size('kl') / 8192 > 20000 AS long_table;

CREATE FUNCTION kl_rows(q text, how text) RETURNS bigint LANGUAGE plpgsql AS $$
DECLARE n bigint := 0; used bool := false; pl text; r record;
BEGIN
	SET LOCAL enable_seqscan = on; SET LOCAL enable_indexscan = on; SET LOCAL enable_indexonlyscan = off; SET LOCAL enable_bitmapscan = on;
	IF how = 'bitmap' THEN SET LOCAL enable_seqscan = off; SET LOCAL enable_indexscan = off;
	ELSIF how = 'index' THEN SET LOCAL enable_seqscan = off; SET LOCAL enable_bitmapscan = off;
	ELSE SET LOCAL enable_indexscan = off; SET LOCAL enable_bitmapscan = off;
	END IF;
	FOR pl IN EXECUTE 'EXPLAIN (COSTS off) ' || q LOOP used := used OR pl LIKE '%kl_i%'; END LOOP;
	IF (how = 'heap') = used THEN RETURN -1; END IF;
	FOR r IN EXECUTE q LOOP n := n + 1; END LOOP;
	RETURN n;
END $$;

-- ranges of 1 day to 400 days (about 100 to 40,000 rows) from 4 starts, with and without deleted_at IS NULL, and on the timestamp
CREATE FUNCTION kl_check(layout text) RETURNS TABLE (layout text, cases int, differ int, below_limit int, above_limit int) LANGUAGE sql AS $$
	WITH q AS (
		SELECT format('SELECT id FROM kl WHERE d BETWEEN %L AND %L%s', a, a + w, CASE WHEN n THEN ' AND del IS NULL' ELSE '' END) AS q
		FROM (SELECT date '2022-01-01' + s * 362 AS a FROM generate_series(0, 3) s) x, unnest(ARRAY[0, 3, 30, 100, 400]) w, (VALUES (true), (false)) v(n)
		UNION ALL
		SELECT format('SELECT id FROM kl WHERE ts >= %L AND ts < %L AND ent = %s', timestamp '2022-01-01' + s * 170 * interval '1 day', timestamp '2022-01-01' + (s * 170 + w) * interval '1 day', 1 + s)
		FROM generate_series(0, 4) s, unnest(ARRAY[1, 20, 200]) w
	), r AS (SELECT q, kl_rows(q, 'heap') AS h FROM q)
	SELECT layout, count(*)::int, count(*) FILTER (WHERE kl_rows(q, 'bitmap') <> h OR kl_rows(q, 'index') <> h)::int,
		count(*) FILTER (WHERE h < 16384)::int, count(*) FILTER (WHERE h >= 16384)::int
	FROM r WHERE h > 0
$$;

CREATE INDEX kl_i ON kl USING sift_col (id, ent, d, ts, del) WITH (key_cut = 'ent', order_columns = 'd, ts', order_array_columns = 'd, ts');
SELECT * FROM kl_check('one segment');
INSERT INTO kl SELECT 105000 + i, 1 + (random() * 59)::int, date '2022-01-01' + (random() * 1500)::int, timestamp '2022-01-01' + random() * 1500 * interval '1 day', NULL, repeat('x', 150) FROM generate_series(1, 10000) i;
UPDATE kl SET d = d + 2 WHERE id % 10 = 0;
SELECT sift.col_promote('kl_i') >= 0 AS promoted;
SELECT * FROM kl_check('promoted');
REINDEX INDEX kl_i;
SELECT * FROM kl_check('reindexed');

DROP FUNCTION kl_check(text);
DROP FUNCTION kl_rows(text, text);
DROP TABLE kl;
DROP EXTENSION pg_sift;
