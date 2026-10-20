-- nine group columns: the scan keeps a key mask as wide as its keys
-- shape: agg
-- expect: agg
-- sets: K400
-- key: -
SELECT o.source, o.status, o.lang, o.country_code, date_trunc('year', o.placed_at)::date AS yr, o.store_id % 3 AS st3,
       o.deleted_at IS NULL AS live, o.customer_id IS NULL AS anon, o.id % 2 AS odd, count(*) AS n, sum(o.total) AS revenue
FROM orders o
WHERE o.store_id IN ({stores})
GROUP BY 1, 2, 3, 4, 5, 6, 7, 8, 9
ORDER BY 1, 2 NULLS FIRST, 3, 4 NULLS FIRST, 5, 6, 7, 8, 9
