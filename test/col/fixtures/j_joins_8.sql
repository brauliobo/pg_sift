-- eight lookup joins, the limit: inner and left joins on a text, an integer and a date leaf and on a pair of columns, a key without a row, a key with two rows and a NULL key
-- shape: join
-- expect: agg
-- sets: K4000 K400
-- key: -
-- scenario: yes
SELECT s.kind, ds.region, dl.family, dc.continent, count(*) AS n, sum(o.total) AS revenue, count(DISTINCT dd.quarter) AS quarters, max(dcu.segment) AS segment,
       min(st.stage) AS stage, max(dp.band) AS band
FROM orders o
JOIN stores s ON s.id = o.store_id
LEFT JOIN dim_source ds ON ds.source = o.source
LEFT JOIN dim_status st ON st.status = o.status
JOIN dim_lang dl ON dl.lang = o.lang
LEFT JOIN dim_country dc ON dc.country_code = o.country_code
LEFT JOIN dim_day dd ON dd.day = o.placed_at
LEFT JOIN dim_customer dcu ON dcu.customer_id = o.customer_id
LEFT JOIN dim_pair dp ON dp.source = o.source AND dp.lang = o.lang
WHERE o.store_id IN ({stores})
GROUP BY 1, 2, 3, 4
ORDER BY 1, 2 NULLS FIRST, 3, 4 NULLS FIRST
