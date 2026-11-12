-- The merges of the overlays of a key_cut index in partitions grow to a partition each and, like the overlays they took, hold rows of every key: a read of a key opens
-- a unit in each. They are not the partitions of the key, and when three of them are alive a merge makes them one set of pieces disjoint in the key, with fewer segments,
-- where they used to wait for the rewrite of the whole index.
CREATE EXTENSION pg_sift;
SET client_min_messages = warning;
SET max_parallel_workers_per_gather = 0;
SET max_parallel_maintenance_workers = 0;
SET sift.async_maintenance = off;
SET sift.vacuum_rewrite_limit = 1;
SET sift.col_auto_merge_partition_kb = 512;
SET sift.col_auto_merge_wal_pct = 5;
SET sift.test_col_segment_kb = 7000;
SET sift.col_build_key_ranges = off;

CREATE TABLE bo (id int, tenant_id int, d date, amount int, pad text) WITH (autovacuum_enabled = false);
INSERT INTO bo SELECT i, (i * 7919) % 400, date '2026-01-01' + (i * 31) % 300, i % 100, md5(i::text) || md5((i + 1)::text) || md5((i + 2)::text) FROM generate_series(1, 168960) i;
VACUUM (FREEZE, ANALYZE) bo;
CREATE INDEX bo_idx ON bo USING sift_col (id, tenant_id, d, amount, pad) WITH (key_cut = 'tenant_id', dict_lookup = off);
SELECT sift.col_maintain('bo_idx') AS partitioned;
CREATE TEMP TABLE bo_partitions AS SELECT gen FROM sift.col_segments('bo_idx') WHERE kind = 'immutable';
-- the segments written since the partitions meet no key twice among themselves: no key is in two of them (two may meet at the one key that ends one and starts the next)
CREATE FUNCTION bo_pieces_disjoint() RETURNS bool LANGUAGE sql AS $$
	WITH r AS (SELECT a.segment, min(a.min::int) lo, max(a.max::int) hi FROM sift.col_unit_agg('bo_idx', 'tenant_id') a
			   WHERE a.segment NOT IN (SELECT gen FROM bo_partitions) GROUP BY a.segment)
	SELECT NOT EXISTS (SELECT 1 FROM r x JOIN r y ON x.segment < y.segment AND x.lo < y.hi AND y.lo < x.hi) $$;

CREATE FUNCTION bo_promote(n int) RETURNS void LANGUAGE plpgsql AS $$
BEGIN
	FOR i IN 1..n LOOP
		INSERT INTO bo SELECT 200000 + (SELECT count(*) FROM bo) + g, (g * 7919) % 400, date '2026-01-01' + (g * 31) % 300, g % 100, md5(g::text) || md5((g + 1)::text) || md5((g + 2)::text) FROM generate_series(1, 12000) g;
		PERFORM sift.col_promote('bo_idx');
	END LOOP;
END $$;
CREATE FUNCTION bo_overlays() RETURNS bigint LANGUAGE sql AS $$ SELECT count(*) FROM sift.col_segments('bo_idx') WHERE kind = 'immutable' AND rows = 12000 $$;

-- two of them wait (a key meets two units of them, as it meets one of the partition); the third makes the merge, which leaves pieces that no key meets twice and ends
SELECT bo_promote(2);
SELECT bo_pieces_disjoint() AS overlays_disjoint;
SELECT bo_overlays() AS overlays, sift.col_merge('bo_idx') AS merge_steps;
SELECT bo_promote(1);
SELECT bo_overlays() AS overlays, sift.col_merge('bo_idx') AS merge_steps;
SELECT bo_overlays() AS overlays, sift.col_merge('bo_idx') AS merge_steps;
SELECT bo_pieces_disjoint() AS pieces_disjoint;
SELECT sift.col_verify('bo_idx', true);
SELECT count(*) = 168960 + 36000 AS rows_kept FROM bo;
DROP FUNCTION bo_promote(int), bo_overlays(), bo_pieces_disjoint();
DROP TABLE bo;
DROP EXTENSION pg_sift;
