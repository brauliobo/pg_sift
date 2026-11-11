-- The segments that promotions and merges of new rows leave over the keys of a key_cut index in partitions are each as large as a partition once they hold half of one, and
-- overlap every partition as the runs of a build do. They are the build's rewrite only when they are most of the index: until then the rewrite that takes them in writes all
-- the partitions they meet for the rows they hold, and is churn, which waits for the WAL budget however many of them there are.
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

CREATE TABLE om (id int, tenant_id int, d date, amount int, pad text) WITH (autovacuum_enabled = false);
INSERT INTO om SELECT i, (i * 7919) % 400, date '2026-01-01' + (i * 31) % 300, i % 100, md5(i::text) || md5((i + 1)::text) || md5((i + 2)::text) FROM generate_series(1, 168960) i;
VACUUM (FREEZE, ANALYZE) om;
CREATE INDEX om_idx ON om USING sift_col (id, tenant_id, d, amount, pad) WITH (key_cut = 'tenant_id', dict_lookup = off);
SELECT sift.col_maintain('om_idx') AS partitioned;

-- overlays of 12,000 rows over all the tenants, each as large as half a partition: the share of the index that they hold, the rewrite due by the automatic rule, and by the manual one
CREATE FUNCTION om_promote(n int) RETURNS void LANGUAGE plpgsql AS $$
BEGIN
	FOR i IN 1..n LOOP
		INSERT INTO om SELECT 200000 + (SELECT count(*) FROM om) + g, (g * 7919) % 400, date '2026-01-01' + (g * 31) % 300, g % 100, md5(g::text) || md5((g + 1)::text) || md5((g + 2)::text) FROM generate_series(1, 12000) g;
		PERFORM sift.col_promote('om_idx');
	END LOOP;
END $$;
CREATE FUNCTION om_state() RETURNS TABLE (overlays bigint, share_pct numeric, due_automatic text, due_manual text) LANGUAGE sql AS $$
	SELECT (SELECT count(*) FROM sift.col_segments('om_idx') WHERE kind = 'immutable' AND rows = 12000),
		   (SELECT round(100.0 * sum(bytes) FILTER (WHERE rows = 12000) / sum(bytes)) FROM sift.col_segments('om_idx') WHERE kind = 'immutable'),
		   coalesce(sift.col_maintenance_due('om_idx', true), '-'), coalesce(sift.col_maintenance_due('om_idx', false), '-') $$;
SELECT om_promote(1);
SELECT * FROM om_state();
SELECT om_promote(4);
SELECT * FROM om_state();

-- the manual rule sees the overlap and the rewrite takes the overlays in
SELECT sift.col_maintain('om_idx') AS rewritten;
SELECT sift.col_verify('om_idx', true);
SELECT * FROM om_state();
DROP FUNCTION om_promote(int), om_state();
DROP TABLE om;
DROP EXTENSION pg_sift;
