-- trap: CUBE next to a set, keys that are expressions, GROUPING() of a subset in the HAVING and the select list
-- shape: trap
-- expect: agg
-- key: -
SELECT o.lang, o.store_id < 50 AS low, extract(year FROM o.placed_at) AS yr, grouping(o.lang) AS g_lang, grouping(o.store_id < 50, o.lang) AS g_two,
       grouping(extract(year FROM o.placed_at), o.lang, o.store_id < 50) AS g_all, count(*) AS n, count(DISTINCT o.source) AS sources, avg(o.store_id) AS avg_store
FROM orders o WHERE o.deleted_at IS NULL
GROUP BY GROUPING SETS (CUBE (o.lang, o.store_id < 50), (extract(year FROM o.placed_at)))
HAVING grouping(o.lang) = 0 OR count(*) > 400
ORDER BY 6, 4, 5, 1 NULLS FIRST, 2 NULLS FIRST, 3 NULLS FIRST
