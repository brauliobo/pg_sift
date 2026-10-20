-- eight joins of which two are a semi and an anti join (EXISTS, NOT EXISTS): the row stays once however many rows match
-- shape: join
-- expect: agg
-- sets: K400
-- key: -
-- scenario: yes
SELECT s.kind, ds.region, dl.family, count(*) AS n, sum(o.total) AS revenue, count(DISTINCT o.id) AS orders, max(dp.band) AS band, min(dcu.segment) AS segment
FROM orders o
JOIN stores s ON s.id = o.store_id
LEFT JOIN dim_source ds ON ds.source = o.source
JOIN dim_lang dl ON dl.lang = o.lang
LEFT JOIN dim_customer dcu ON dcu.customer_id = o.customer_id
LEFT JOIN dim_pair dp ON dp.source = o.source AND dp.lang = o.lang
WHERE o.store_id IN ({stores})
  AND EXISTS (SELECT 1 FROM dim_country dc WHERE dc.country_code = o.country_code)
  AND NOT EXISTS (SELECT 1 FROM dim_status st WHERE st.status = o.status AND st.stage = 9)
GROUP BY 1, 2, 3
ORDER BY 1, 2 NULLS FIRST, 3
