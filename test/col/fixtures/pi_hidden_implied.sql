-- a partial column index that does not hold the column of its predicate: the conditions that imply the predicate are answered without it
-- shape: flat
-- expect: agg
-- key: 1
SELECT grp, count(*) AS n, count(amount) AS priced, sum(amount) AS total, avg(amount) AS mean
FROM t_partial_hidden
WHERE status = 'open' AND grp <> 3 AND NOT flag
GROUP BY grp ORDER BY grp
