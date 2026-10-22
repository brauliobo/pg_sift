-- nine lookup joins: one more than j_joins_8, the same result as the normal plan
-- shape: join
-- expect: agg
-- sets: K400
-- key: -
SELECT s.kind, ds.region, dl.family, dc.continent, count(*) AS n, sum(o.total) AS revenue, count(DISTINCT dd.quarter) AS quarters, max(dcu.segment) AS segment,
       min(st.stage) AS stage, max(dp.band) AS band, max(dc2.segment) AS segment2
FROM orders o
JOIN tenants s ON s.id = o.tenant_id
LEFT JOIN dim_source ds ON ds.source = o.source
LEFT JOIN dim_status st ON st.status = o.status
JOIN dim_lang dl ON dl.lang = o.lang
LEFT JOIN dim_country dc ON dc.country_code = o.country_code
LEFT JOIN dim_day dd ON dd.day = o.placed_at
LEFT JOIN dim_customer dcu ON dcu.customer_id = o.customer_id
LEFT JOIN dim_customer dc2 ON dc2.customer_id = o.customer_id
LEFT JOIN dim_pair dp ON dp.source = o.source AND dp.lang = o.lang
WHERE o.tenant_id IN ({tenants})
GROUP BY 1, 2, 3, 4
ORDER BY 1, 2 NULLS FIRST, 3, 4 NULLS FIRST
