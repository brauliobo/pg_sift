-- a common table expression that two aggregates read is planned inline when the scans are the cheaper part
-- label: `WITH s AS (.. GROUP BY parent_id, kind) SELECT (SELECT sum(a) FROM s), (SELECT count(*) FROM s WHERE n > 100)`
--@ pg
WITH s AS (SELECT parent_id, kind, count(*) AS n, sum(amount) AS a FROM events WHERE deleted_at IS NULL GROUP BY 1, 2)
SELECT (SELECT sum(a) FROM s) AS total, (SELECT count(*) FROM s WHERE n > 100) AS big
