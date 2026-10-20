-- ROLLUP of eight columns (the set number is the ninth key): the scan keeps a key mask as wide as its keys
-- shape: agg
-- expect: agg
-- sets: K400
-- key: -
SELECT o.source, o.status, o.lang, o.country_code, o.store_id % 3 AS st3, o.deleted_at IS NULL AS live, o.customer_id IS NULL AS anon, o.id % 2 AS odd,
       grouping(o.source, o.status, o.lang, o.country_code, o.store_id % 3, o.deleted_at IS NULL, o.customer_id IS NULL, o.id % 2) AS g, count(*) AS n
FROM orders o
WHERE o.store_id IN ({stores})
GROUP BY ROLLUP (o.source, o.status, o.lang, o.country_code, o.store_id % 3, o.deleted_at IS NULL, o.customer_id IS NULL, o.id % 2)
ORDER BY g, 1 NULLS FIRST, 2 NULLS FIRST, 3 NULLS FIRST, 4 NULLS FIRST, 5 NULLS FIRST, 6 NULLS FIRST, 7 NULLS FIRST, 8 NULLS FIRST
