-- a partial column index (not deleted, from the eighth day): conditions that imply its predicate are answered by it
-- shape: flat
-- expect: agg
-- key: 1
SELECT grp, count(*) AS n, count(DISTINCT tag) AS tags, sum(amount) AS total, avg(amount) AS mean
FROM t_partial
WHERE deleted_at IS NULL AND day >= '2026-01-20' AND grp <> 3
GROUP BY grp ORDER BY grp
