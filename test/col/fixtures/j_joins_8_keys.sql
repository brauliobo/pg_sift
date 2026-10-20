-- eight lookup joins whose columns are all group keys, a HAVING and a filter on a joined column
-- shape: join
-- expect: agg
-- sets: K400
-- key: -
-- scenario: yes
SELECT s.kind, ds.region, dl.family, dc.continent, dd.quarter, dcu.segment, st.stage, dp.band, count(*) AS n, sum(o.total) AS revenue
FROM orders o
JOIN stores s ON s.id = o.store_id
LEFT JOIN dim_source ds ON ds.source = o.source
LEFT JOIN dim_status st ON st.status = o.status
LEFT JOIN dim_lang dl ON dl.lang = o.lang
LEFT JOIN dim_country dc ON dc.country_code = o.country_code
LEFT JOIN dim_day dd ON dd.day = o.placed_at
LEFT JOIN dim_customer dcu ON dcu.customer_id = o.customer_id
LEFT JOIN dim_pair dp ON dp.source = o.source AND dp.lang = o.lang
WHERE o.store_id IN ({stores}) AND (dc.continent IS NULL OR dc.continent <> 'west')
GROUP BY 1, 2, 3, 4, 5, 6, 7, 8
HAVING count(*) > 1
ORDER BY 1, 2 NULLS FIRST, 3 NULLS FIRST, 4 NULLS FIRST, 5 NULLS FIRST, 6 NULLS FIRST, 7 NULLS FIRST, 8 NULLS FIRST
