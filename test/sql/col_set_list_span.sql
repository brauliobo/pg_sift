-- An IN list tested against the keys a unit keeps (set summaries: the list, the runs, the bitmap, the bloom filter) skips the units whose keys cannot meet the
-- list: lists below, above and between the keys of the units, on their edges and across them give the heap's rows, and the same units are read as before the list was
-- cut to the keys of the unit's range.
CREATE EXTENSION pg_sift_test CASCADE;
CREATE EXTENSION pg_sift_helpers;
SET client_min_messages = warning;
SET max_parallel_workers_per_gather = 0;
SET max_parallel_maintenance_workers = 0;
SET statement_timeout = '120s';

-- o4: bursts of 250 consecutive keys far apart (runs, where a bitmap or a list of offsets would not fit), o1: a few owners to a unit (a short list), o2: bursts of consecutive ids (runs), o3: sparse keys that are many per unit (a bloom filter or a long list)
CREATE TABLE sl (id int PRIMARY KEY, o1 int NOT NULL, o2 int NOT NULL, o3 int NOT NULL, o4 int NOT NULL, val int, pad text) WITH (autovacuum_enabled = off);
ALTER TABLE sl ALTER COLUMN pad SET STORAGE PLAIN;
CREATE INDEX sl_i ON sl USING sift_col (id, o1, o2, o3, o4, val) WITH (write_buffer_limit = 4096, key_cut = 'o1', key_columns = 'o1, o2, o3, o4');
SET sift.col_auto_merge = off;
-- forty batches of consecutive ids, one segment each: the keys of a unit are the keys of its batch
DO $$
BEGIN
	FOR b IN 0..39 LOOP
		INSERT INTO sl SELECT g, 1 + (hashint4(g / 50) & 2147483647) % 600, g / 20 - 500, (hashint4(g) & 2147483647) % 2000000 - 1000000, (g / 250) * 100000 + g % 250, g % 1000, repeat('x', 100)
		FROM generate_series(b * 1500 + 1, b * 1500 + 1500) g;
		PERFORM sift.col_promote('sl_i');
	END LOOP;
END $$;
VACUUM (ANALYZE, INDEX_CLEANUP off) sl;
SELECT sift.col_verify('sl_i', true);

CREATE FUNCTION sl_rows(q text, push bool) RETURNS text LANGUAGE sql AS $$ SELECT sift_helpers.rows_agg_four_scans_or_no_rows(q, push) $$;
CREATE FUNCTION sl_units(q text) RETURNS int LANGUAGE plpgsql AS $$
DECLARE l text; n int;
BEGIN
	PERFORM set_config('sift.enable_agg', 'on', true);
	PERFORM set_config(s, 'false', true) FROM unnest(ARRAY['enable_seqscan', 'enable_indexscan', 'enable_bitmapscan', 'enable_indexonlyscan']) s;
	FOR l IN EXECUTE 'EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS OFF) ' || q LOOP
		n := coalesce(substring(l from '^ *Units: (\d+) read')::int, n);
	END LOOP;
	RETURN n;
END $$;

CREATE TABLE sl_q (n int, q text);
INSERT INTO sl_q VALUES
	(1, 'SELECT count(*), sum(val) FROM sl WHERE o1 IN (1, 2, 3)'),
	(2, 'SELECT count(*), sum(val) FROM sl WHERE o1 IN (0, 601, 100000, -5)'),
	(3, 'SELECT count(*), sum(val) FROM sl WHERE o1 IN (-3, 0, 300, 301, 302, 700, 900)'),
	(4, 'SELECT count(*), sum(val) FROM sl WHERE o1 IN (' || (SELECT string_agg(g::text, ', ') FROM generate_series(10, 600, 8) g) || ')'),
	(5, 'SELECT count(*), sum(val) FROM sl WHERE o1 IN (' || (SELECT string_agg(g::text, ', ') FROM generate_series(1, 600, 2) g) || ')'),
	(6, 'SELECT count(*), sum(val) FROM sl WHERE o2 IN (-500, -499, 0, 1, 2, 2499, 2500)'),
	(7, 'SELECT count(*), sum(val) FROM sl WHERE o2 IN (-501, 2501, 4000, -4000)'),
	(8, 'SELECT count(*), sum(val) FROM sl WHERE o2 IN (' || (SELECT string_agg(g::text, ', ') FROM generate_series(-500, 2500, 37) g) || ')'),
	(9, 'SELECT count(*), sum(val) FROM sl WHERE o2 IN (' || (SELECT string_agg(g::text, ', ') FROM generate_series(100, 2400, 300) g) || ')'),
	(10, 'SELECT count(*), sum(val) FROM sl WHERE o3 IN (' || (SELECT string_agg(o3::text, ', ' ORDER BY id) FROM sl WHERE id % 997 = 0) || ')'),
	(11, 'SELECT count(*), sum(val) FROM sl WHERE o3 IN (-1000001, 1000001, 0, 5, 777777)'),
	(12, 'SELECT count(*), sum(val) FROM sl WHERE o3 IN (' || (SELECT string_agg(o3::text, ', ' ORDER BY id) FROM sl WHERE id % 211 = 0) || ') AND o1 IN (5, 6, 7, 400)'),
	(13, 'SELECT count(*), sum(val) FROM sl WHERE o1 IN (10, 20, 30) OR o2 IN (5, 6)'),
	(14, 'SELECT count(*), sum(val) FROM sl WHERE o2 IN (' || (SELECT string_agg(g::text, ', ') FROM generate_series(-500, 2500, 1) g) || ')'),
	(15, 'SELECT count(*), sum(val) FROM sl WHERE o4 IN (-1, 249, 250, 99999, 100000, 100249, 100250, 23900000, 23900249, 23900250, 30000000)'),
	(16, 'SELECT count(*), sum(val) FROM sl WHERE o4 IN (' || (SELECT string_agg(k::text, ', ') FROM generate_series(0, 24000000, 1300000) k) || ')'),
	(17, 'SELECT count(*), sum(val) FROM sl WHERE o4 IN (' || (SELECT string_agg(((g / 250) * 100000 + g % 250 + 1)::text, ', ' ORDER BY g) FROM generate_series(1, 60000, 4999) g) || ')');

SELECT n, sl_rows(q, true) = sl_rows(q, false) AS same, sl_units(q) AS units FROM sl_q ORDER BY n;

DROP TABLE sl_q;
DROP FUNCTION sl_rows(text, bool), sl_units(text);
DROP TABLE sl;
DROP EXTENSION pg_sift_helpers;
DROP EXTENSION pg_sift_test, pg_sift CASCADE;
