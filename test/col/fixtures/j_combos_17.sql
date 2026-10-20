-- trap: seventeen combos, one more than the limit (eight joins and nine filters over other leaves): the normal plan
-- shape: trap
-- expect: plain
-- sets: K400
-- key: -
SELECT s.kind, ds.region, dl.family, dc.continent, count(*) AS n, sum(o.total) AS revenue, count(DISTINCT dd.quarter) AS quarters, max(dcu.segment) AS segment,
       min(st.stage) AS stage, max(dp.band) AS band
FROM orders o
JOIN tenants s ON s.id = o.tenant_id
LEFT JOIN dim_source ds ON ds.source = o.source
LEFT JOIN dim_status st ON st.status = o.status
JOIN dim_lang dl ON dl.lang = o.lang
LEFT JOIN dim_country dc ON dc.country_code = o.country_code
LEFT JOIN dim_day dd ON dd.day = o.placed_at
LEFT JOIN dim_customer dcu ON dcu.customer_id = o.customer_id
LEFT JOIN dim_pair dp ON dp.source = o.source AND dp.lang = o.lang
WHERE o.tenant_id IN ({tenants})
  AND (length(o.source) + length(coalesce(o.status, ''))) % 5 <> 1
  AND (length(o.source) + length(o.lang)) % 7 <> 3
  AND (length(o.source) + length(coalesce(o.country_code, ''))) % 6 <> 2
  AND (length(coalesce(o.status, '')) + length(o.lang)) % 4 <> 1
  AND (length(coalesce(o.status, '')) + length(coalesce(o.country_code, ''))) % 5 <> 4
  AND (length(o.lang) + length(coalesce(o.country_code, ''))) % 3 <> 2
  AND (o.tenant_id + length(o.lang)) % 9 <> 4
  AND (o.tenant_id + length(coalesce(o.status, ''))) % 11 <> 5
  AND (o.tenant_id + length(coalesce(o.country_code, ''))) % 13 <> 6
GROUP BY 1, 2, 3, 4
ORDER BY 1, 2 NULLS FIRST, 3, 4 NULLS FIRST
