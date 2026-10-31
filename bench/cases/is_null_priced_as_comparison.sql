-- IS NULL and IS NOT NULL on a column of the index are priced as a comparison, a bit test of each row, not as a pattern test: the scan beats the bitmap scan of the b-tree on the key
-- label: `set_id = 7 AND closed_at IS NOT NULL`, count of 300,000 rows of 10 sets, a b-tree on `set_id` alone
--@ setup
DO $do$ BEGIN
	IF to_regclass('c_owned') IS NULL THEN
		CREATE TABLE c_owned WITH (autovacuum_enabled = false) AS
		SELECT i AS id, 1 + i % 10 AS set_id, CASE WHEN i % 3 = 0 THEN timestamp '2024-01-01' + i * interval '7 seconds' END AS closed_at
		FROM generate_series(1, 300000) i;
		CREATE INDEX c_owned_set ON c_owned (set_id);
		CREATE INDEX c_owned_col ON c_owned USING sift_col (id, set_id, closed_at);
	END IF;
END $do$;
VACUUM (FREEZE, ANALYZE) c_owned
--@ pg
SELECT count(*) FROM c_owned WHERE set_id = 7 AND closed_at IS NOT NULL
