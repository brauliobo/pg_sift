-- The segments of a key-cut index that are as large as partitions and overlap in the key are the runs of a build only when taking them out would leave most of the index:
-- the cheapest set of them whose removal leaves none overlapping, in bytes, is half of the index or more (two equal runs are each half). Pieces that tier merges leave over
-- a base partition, each over a slice of its keys, are churn however many meet the base: removing the pieces (or the base, when more bytes than they are) is the cover, and
-- it is under half. Their rewrite waits for the WAL budget, which an automatic check says by finding nothing due while the manual one (col_maintain) still sees the overlap.
-- Three geometries: the runs a build leaves (due at once), one base partition with pieces added one by one (never due), partitions with overlays as wide as their keys.
CREATE EXTENSION pg_sift;
SET client_min_messages = warning;
SET max_parallel_workers_per_gather = 0;
SET max_parallel_maintenance_workers = 0;
SET sift.async_maintenance = off;
SET sift.vacuum_rewrite_limit = 1;
SET sift.col_auto_merge_partition_kb = 512;
SET sift.col_auto_merge_wal_pct = 5;
SET sift.col_build_key_ranges = off;

CREATE TABLE kc (id int, tenant_id int, d date, amount int, pad text) WITH (autovacuum_enabled = false);
CREATE FUNCTION kc_fill(n int, lo int, hi int) RETURNS void LANGUAGE sql AS $$
	INSERT INTO kc SELECT 200000 + (SELECT count(*) FROM kc) + g, lo + (g * 7919) % (hi - lo), date '2026-01-01' + (g * 31) % 300, g % 100, md5(g::text) || md5((g + 1)::text) || md5((g + 2)::text) FROM generate_series(1, n) g $$;
CREATE FUNCTION kc_add(n int, lo int, hi int) RETURNS void LANGUAGE plpgsql AS $$
BEGIN
	PERFORM kc_fill(n, lo, hi);
	PERFORM sift.col_promote('kc_idx');
END $$;
-- the segments, the share of the index bytes that the added ones (11,000 rows) are, and the rewrite due by the automatic rule and by the manual one
CREATE FUNCTION kc_state() RETURNS TABLE (segments bigint, added_pct numeric, due_automatic text, due_manual text) LANGUAGE plpgsql AS $$
BEGIN
	RETURN QUERY SELECT count(*), round(100.0 * coalesce(sum(bytes) FILTER (WHERE rows = 11000), 0) / sum(bytes)),
		   coalesce(sift.col_maintenance_due('kc_idx', true), '-'), coalesce(sift.col_maintenance_due('kc_idx', false), '-')
	FROM sift.col_segments('kc_idx') WHERE kind = 'immutable';
END $$;

-- the runs of a build that outgrew the memory of a segment writer: three of 11,000 to 14,000 rows, each over every key
SET sift.test_col_segment_kb = 1500;
SELECT kc_fill(40000, 0, 400);
VACUUM (FREEZE, ANALYZE) kc;
CREATE INDEX kc_idx ON kc USING sift_col (id, tenant_id, d, amount, pad) WITH (key_cut = 'tenant_id', dict_lookup = off);
SELECT count(*) AS runs FROM sift.col_segments('kc_idx') WHERE kind = 'immutable' AND rows > 10000;
SELECT segments, due_automatic, due_manual FROM kc_state();
DROP INDEX kc_idx;
TRUNCATE kc;

-- one base partition (an index that is one segment) and a piece of 11,000 rows over a slice of the keys each time, from one to eight: the pieces are the cover, then the base is
SET sift.test_col_segment_kb = 8000;
SELECT kc_fill(40000, 0, 400);
VACUUM (FREEZE, ANALYZE) kc;
CREATE INDEX kc_idx ON kc USING sift_col (id, tenant_id, d, amount, pad) WITH (key_cut = 'tenant_id', dict_lookup = off);
SELECT stmt FROM generate_series(1, 8) i, LATERAL (VALUES (1, format('SELECT kc_add(11000, %s, %s)', (i - 1) * 50, i * 50)), (2, 'SELECT * FROM kc_state()')) v(k, stmt) ORDER BY i, k \gexec
DROP INDEX kc_idx;
TRUNCATE kc;

-- partitions of the key and an overlay of 11,000 rows over every key at a time: the overlay meets the most, it is the cover
SET sift.test_col_segment_kb = 1500;
SELECT kc_fill(40000, 0, 400);
VACUUM (FREEZE, ANALYZE) kc;
CREATE INDEX kc_idx ON kc USING sift_col (id, tenant_id, d, amount, pad) WITH (key_cut = 'tenant_id', dict_lookup = off);
SELECT sift.col_maintain('kc_idx') AS partitioned;
SELECT count(*) > 1 AS partitions FROM sift.col_segments('kc_idx') WHERE kind = 'immutable';
SELECT stmt FROM generate_series(1, 3) i, LATERAL (VALUES (1, 'SELECT kc_add(11000, 0, 400)'), (2, 'SELECT * FROM kc_state()')) v(k, stmt) ORDER BY i, k \gexec

-- the manual rule takes the overlays in, and nothing is due after it; the rows are all there
SELECT sift.col_maintain('kc_idx') AS rewritten;
SELECT sift.col_verify('kc_idx', true);
SELECT count(*) FILTER (WHERE rows = 11000) AS overlays, coalesce(sift.col_maintenance_due('kc_idx', true), '-') AS due_automatic FROM sift.col_segments('kc_idx') WHERE kind = 'immutable';
SELECT count(*) = 40000 + 3 * 11000 AS rows_kept FROM kc;
DROP FUNCTION kc_fill(int, int, int), kc_add(int, int, int), kc_state();
DROP TABLE kc;
DROP EXTENSION pg_sift;
