-- sift.explain_why for ORDER BY .. LIMIT: a b-tree that delivers the order reads only the rows asked for, so its plan beats the scan's top-N path after the LIMIT
-- is costed; the reason names the two costs (before, the row said the scan was used while the plan was the b-tree's).
CREATE EXTENSION pg_sift;
SET max_parallel_workers_per_gather = 0;
CREATE TABLE readings AS
SELECT i % 20 AS device, TIMESTAMP '2025-01-01' + (i / 20) * interval '1 minute' AS ts, (i * 7919 % 10007) AS value
FROM generate_series(1, 200000) i;
CREATE INDEX readings_device_ts ON readings (device, ts);
CREATE INDEX readings_col ON readings USING sift_col (device, ts, value);
VACUUM (FREEZE, ANALYZE) readings;
\ir test/agg_helpers.sql
CREATE FUNCTION topn_why(q text) RETURNS TABLE (used bool, slug text, says_limit bool) LANGUAGE sql AS
$$ SELECT w.used, w.slug, w.reason LIKE '%the normal plan with the LIMIT%' FROM sift.explain_why(q) w WHERE w.arm = 'topn' $$;
-- the b-tree on (device, ts) reads 100 rows
SELECT * FROM topn_why($q$ SELECT ts, value FROM readings WHERE device = 7 ORDER BY ts DESC LIMIT 100 $q$);
-- no b-tree gives the order of value: the scan's top-N is the plan
SELECT * FROM topn_why($q$ SELECT device, ts, value FROM readings ORDER BY value DESC, ts LIMIT 100 $q$);
DROP FUNCTION topn_why(text), outcome(text, bool), runs_in_scan(text), pushed(text), runq(text, bool);
DROP TABLE readings;
DROP EXTENSION pg_sift;
