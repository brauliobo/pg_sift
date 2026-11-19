-- An index as 5.0 wrote it has no recorded partitions (sift.test_col_record_partitions = off makes one): the segments of the size of a partition that no two meet in. The first maintenance step
-- records them with one directory update: no segment is written, no generation changes, the deletion records pending on its units stay where they are, and the answers are the heap's before and after.
CREATE EXTENSION pg_sift;
CREATE EXTENSION pg_sift_test CASCADE;
SET client_min_messages = warning;
SET max_parallel_workers_per_gather = 0;
SET max_parallel_maintenance_workers = 0;
SET sift.async_maintenance = off;
SET sift.test_col_segment_kb = 1024;
SET sift.col_auto_merge_partition_kb = 512;
SET sift.test_col_net = off;
SET statement_timeout = '120s';

CREATE TABLE ad (id int PRIMARY KEY, dim int NOT NULL, day date NOT NULL, v int, w int) WITH (autovacuum_enabled = false);
INSERT INTO ad SELECT g, 1 + (g::bigint * 7919 % 12)::int, date '2018-01-01' + (((g::bigint * 2654435761) % 4294967296 / 65536) % 2900)::int, g % 100, (g::bigint * 40503 % 1000003)::int FROM generate_series(1, 400000) g;
SET sift.test_col_record_partitions = off;
CREATE INDEX ad_idx ON ad USING sift_col (id, dim, day, v, w) WITH (key_cut = 'dim, day', track_changes = 'id', dead_rewrite_pct = 100);
VACUUM (FREEZE, ANALYZE) ad;
SELECT sift.col_merge('ad_idx', true) >= 0 AS merged;
RESET sift.test_col_record_partitions;
SELECT count(*) > 10 AS partition_sized_segments, (SELECT count(*) FROM sift._test_dir_parts('ad_idx')) AS recorded FROM sift.col_segments('ad_idx') WHERE kind = 'immutable' AND bytes >= 256 * 1024;

CREATE FUNCTION chk(OUT same_as_heap bool) LANGUAGE plpgsql AS $$
DECLARE a text; o text; q text;
BEGIN
	same_as_heap := true;
	FOREACH q IN ARRAY ARRAY[
		'SELECT dim, count(*), sum(v) FROM ad GROUP BY 1 ORDER BY 1',
		'SELECT count(*), sum(w), min(id), max(id) FROM ad',
		'SELECT count(*) FROM ad WHERE dim BETWEEN 3 AND 8 AND v < 50',
		'SELECT id FROM ad WHERE dim = 7 AND day = date ''2020-06-01'' ORDER BY id']
	LOOP
		PERFORM set_config('sift.enable_agg', 'on', true);
		EXECUTE 'SELECT string_agg(r::text, '';'') FROM (' || q || ') r' INTO a;
		PERFORM set_config('sift.enable_agg', 'off', true);
		EXECUTE 'SELECT string_agg(r::text, '';'') FROM (' || q || ') r' INTO o;
		same_as_heap := same_as_heap AND a IS NOT DISTINCT FROM o;
	END LOOP;
END $$;
CREATE FUNCTION pending() RETURNS int LANGUAGE sql AS $$ SELECT count(*)::int FROM sift._test_col_log_entries('ad_idx') $$;

-- deleted rows whose records are not folded yet
DELETE FROM ad WHERE id % 40 = 1;
SELECT pending() > 0 AS records_pending;
CREATE TABLE before AS SELECT gen, units, bytes, rows, pending() AS entries FROM sift.col_segments('ad_idx') WHERE kind = 'immutable';
SELECT * FROM chk();

SELECT sift._test_col_adopt('ad_idx') AS adopted;
SELECT count(*) AS partitions, count(*) FILTER (WHERE lo_empty) AS empty_bound, bool_and(rows_from_lo) AS rows_from_lo, bool_and(rows_below_next) AS rows_below_next FROM sift._test_col_part_bounds('ad_idx');
SELECT (SELECT count(*) FROM sift._test_dir_parts('ad_idx')) = (SELECT count(*) FROM sift._test_col_part_bounds('ad_idx')) AS same_partitions;
-- the segments are the same ones: same generations, units, bytes and rows; the records are where they were
SELECT NOT EXISTS (SELECT 1 FROM before b FULL JOIN sift.col_segments('ad_idx') s ON s.gen = b.gen AND s.kind = 'immutable' WHERE s.kind = 'immutable' AND (b.gen IS NULL OR s.units IS DISTINCT FROM b.units OR s.bytes IS DISTINCT FROM b.bytes OR s.rows IS DISTINCT FROM b.rows)) AS same_segments,
	(SELECT count(*) FROM before) = (SELECT count(*) FROM sift.col_segments('ad_idx') WHERE kind = 'immutable') AS same_count, pending() = (SELECT max(entries) FROM before) AS same_records;
SELECT * FROM chk();
SELECT sift.col_verify('ad_idx', true);
SELECT sift._test_col_adopt('ad_idx') AS adopted_again;

-- the rolling rewrite works on the partitions it found recorded, and keeps the bounds
DELETE FROM ad WHERE id % 5 = 2;
VACUUM (FREEZE, DISABLE_PAGE_SKIPPING, INDEX_CLEANUP ON) ad;
SELECT sift.col_maintain('ad_idx') AS rolled;
SELECT count(*) > 10 AS partitions, bool_and(rows_from_lo) AS rows_from_lo, bool_and(rows_below_next) AS rows_below_next FROM sift._test_col_part_bounds('ad_idx');
SELECT * FROM chk();
SELECT sift.col_verify('ad_idx', true);

-- the maintenance adopts by itself: a merge step on an index without recorded partitions records them first
CREATE TABLE ae (id int PRIMARY KEY, dim int NOT NULL, day date NOT NULL, v int, w int) WITH (autovacuum_enabled = false);
INSERT INTO ae SELECT g, 1 + (g::bigint * 7919 % 12)::int, date '2018-01-01' + (((g::bigint * 2654435761) % 4294967296 / 65536) % 2900)::int, g % 100, (g::bigint * 40503 % 1000003)::int FROM generate_series(1, 400000) g;
SET sift.test_col_record_partitions = off;
CREATE INDEX ae_idx ON ae USING sift_col (id, dim, day, v, w) WITH (key_cut = 'dim, day');
SELECT sift.col_merge('ae_idx', true) >= 0 AS merged;
RESET sift.test_col_record_partitions;
SELECT (SELECT count(*) FROM sift._test_dir_parts('ae_idx')) = 0 AS none_recorded;
SELECT sift.col_merge('ae_idx') >= 0 AS stepped;
SELECT (SELECT count(*) FROM sift._test_dir_parts('ae_idx')) > 10 AS recorded_by_the_step;
SELECT bool_and(rows_from_lo) AS rows_from_lo, bool_and(rows_below_next) AS rows_below_next FROM sift._test_col_part_bounds('ae_idx');

DROP FUNCTION chk(), pending();
DROP TABLE before, ad, ae;
DROP EXTENSION pg_sift_test;
DROP EXTENSION pg_sift;
