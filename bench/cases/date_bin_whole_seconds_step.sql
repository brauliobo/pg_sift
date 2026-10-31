-- a key date_bin(stride, ts, origin) of whole seconds is a step of its own, like date_trunc: the scan evaluates it once per step of the data
-- label: `date_bin('15 minutes', started_at, timestamp '2000-01-01')`, count by bucket over 300,000 jobs of 9 seconds apart
-- ordered: yes
--@ setup
DO $do$ BEGIN
	IF to_regclass('c_jobs') IS NULL THEN
		CREATE TABLE c_jobs WITH (autovacuum_enabled = false) AS
		SELECT i AS id, ts + (hashint4(i) & 255) * interval '1 second' AS started_at, ts AS queued_at,
		       ts + ((hashint4(i) & 255) + 60 + (hashint4(i + 5) & 1023)) * interval '1 second' AS done_at, (ARRAY['a', 'b', 'c', 'd'])[1 + i % 4] AS kind
		FROM (SELECT i, timestamp '2024-01-01' + i * interval '9 seconds' AS ts FROM generate_series(1, 300000) i) s;
		CREATE INDEX c_jobs_col ON c_jobs USING sift_col (id, queued_at, started_at, done_at, kind);
	END IF;
END $do$;
VACUUM (FREEZE, ANALYZE) c_jobs
--@ pg
SELECT date_bin('15 minutes', started_at, timestamp '2000-01-01') AS bucket, count(*) AS n FROM c_jobs GROUP BY 1 ORDER BY 1
