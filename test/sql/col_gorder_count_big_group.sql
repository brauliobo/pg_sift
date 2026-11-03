-- count(*) WHERE g = x of the biggest group (a third of the table) of a key_order = 'g, ts' index after updates spread over every unit and a VACUUM: the units answer from their
-- pre-aggregates net of their dead rows; the entries of the group (a liveness test each) cost more than the units here, so the count does not take them.
CREATE EXTENSION pg_sift_test CASCADE;
SET sift.async_maintenance = off;
SET sift.col_auto_merge = off;
SET max_parallel_maintenance_workers = 0;
SET max_parallel_workers_per_gather = 0;
SET client_min_messages = warning;
\set VERBOSITY terse

CREATE TABLE bg (id int, g int, ts timestamp, score int, pad text);
ALTER TABLE bg ALTER COLUMN pad SET STORAGE PLAIN;
INSERT INTO bg SELECT i, 1 + least(29, floor(30 * power((hashint4(i) & 2147483647) / 2147483647.0, 3)))::int, timestamp '2023-01-01' + i * interval '20 seconds',
	   abs(hashint4(i + 5)) % 5000, md5(i::text) || md5(i::text || 'x') || md5(i::text || 'y')
FROM generate_series(1, 150000) i;
CREATE INDEX bg_i ON bg USING sift_col (id, g, ts, score, pad) WITH (key_order = 'g, ts');
VACUUM (FREEZE, ANALYZE, DISABLE_PAGE_SKIPPING) bg;
UPDATE bg SET pad = 'u' || pad WHERE id % 10 = 3;
VACUUM bg;

-- the units that net their dead rows out of the pre-aggregates, for the biggest group; every group agrees with the heap
CREATE FUNCTION bg_chk(q text) RETURNS text LANGUAGE plpgsql AS $$
DECLARE pl text; net int := 0; ra text; rb text;
BEGIN
	SET LOCAL sift.enable_agg = on; SET LOCAL sift.enable_col_scan = on; SET LOCAL enable_indexscan = off; SET LOCAL enable_bitmapscan = off; SET LOCAL enable_indexonlyscan = off;
	SET LOCAL sift.agg_force = on; SET LOCAL enable_seqscan = off; SET LOCAL enable_sort = off;
	FOR pl IN EXECUTE 'EXPLAIN (ANALYZE, COSTS off, TIMING off, SUMMARY off) ' || q LOOP net := net + coalesce(substring(pl from '([0-9]+) net of dead rows')::int, 0); END LOOP;
	EXECUTE q INTO ra;
	SET LOCAL sift.enable_agg = off; SET LOCAL enable_seqscan = on; SET LOCAL enable_sort = on;
	EXECUTE q INTO rb;
	RETURN CASE WHEN ra IS DISTINCT FROM rb THEN 'DIFFERS ' || q WHEN net > 0 THEN 'net of dead rows' ELSE 'entries' END;
END $$;
SELECT bg_chk('SELECT count(*) FROM bg WHERE g = 1') AS biggest;
SELECT count(*) FILTER (WHERE bg_chk(format('SELECT count(*) FROM bg WHERE g = %s', g)) LIKE 'DIFFERS%') AS differing FROM generate_series(1, 31) g;
SELECT sift.col_verify('bg_i');

DROP TABLE bg;
DROP FUNCTION bg_chk(text);
DROP EXTENSION pg_sift_test, pg_sift CASCADE;
