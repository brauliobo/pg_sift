-- CUBE of six columns: 64 sets over seven keys with a HAVING on GROUPING()
-- shape: sets
-- expect: agg
-- sets: K400
-- key: -
SELECT o.source, o.status, o.lang, o.country_code, o.store_id % 3 AS st3, o.deleted_at IS NULL AS live,
       grouping(o.source, o.status, o.lang, o.country_code, o.store_id % 3, o.deleted_at IS NULL) AS g, count(*) AS n, sum(o.total) AS revenue
FROM orders o
WHERE o.store_id IN ({stores})
GROUP BY CUBE (o.source, o.status, o.lang, o.country_code, o.store_id % 3, o.deleted_at IS NULL)
HAVING grouping(o.source) = 1 OR count(*) > 3
ORDER BY g, 1 NULLS FIRST, 2 NULLS FIRST, 3 NULLS FIRST, 4 NULLS FIRST, 5 NULLS FIRST, 6 NULLS FIRST
