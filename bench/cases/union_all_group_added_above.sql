-- a GROUP BY over a UNION ALL of selects groups each arm that runs in the scan and adds the groups up above the union: a queue's depth over time from three timestamps of the same rows
-- label: three arms (queued +1, started -1, done 0) of `date_bin('15 minutes', ..)`, `sum` of the steps by bucket over 300,000 jobs
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
SELECT bucket, sum(step) AS depth FROM (
	SELECT date_bin('15 minutes', queued_at, timestamp '2000-01-01') AS bucket, 1 AS step FROM c_jobs
	UNION ALL SELECT date_bin('15 minutes', started_at, timestamp '2000-01-01'), -1 FROM c_jobs
	UNION ALL SELECT date_bin('15 minutes', done_at, timestamp '2000-01-01'), 0 FROM c_jobs) u
GROUP BY 1 ORDER BY 1
