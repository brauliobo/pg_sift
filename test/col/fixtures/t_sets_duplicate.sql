-- trap: a set written twice gives its rows twice
-- shape: trap
-- expect: agg
-- key: -
SELECT o.lang, grouping(o.lang) AS g, count(*) AS n, max(o.placed_at) AS last_day
FROM orders o WHERE o.store_id < 120 AND o.deleted_at IS NULL
GROUP BY GROUPING SETS ((o.lang), (o.lang), ()) ORDER BY 2, 1 NULLS FIRST
