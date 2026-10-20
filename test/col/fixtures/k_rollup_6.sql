-- ROLLUP of six columns: seven sets, the set number is the seventh key; NULL data and NULL totals tell apart through GROUPING()
-- shape: sets
-- expect: agg
-- sets: K4000 K400
-- key: -
-- scenario: yes
SELECT o.source, o.status, o.lang, o.country_code, o.tenant_id % 3 AS st3, o.deleted_at IS NULL AS live, grouping(o.source, o.status, o.lang, o.country_code, o.tenant_id % 3, o.deleted_at IS NULL) AS g,
       count(*) AS n, sum(o.total) AS revenue, count(*) FILTER (WHERE o.discount > 20) AS discounted
FROM orders o
WHERE o.tenant_id IN ({tenants})
GROUP BY ROLLUP (o.source, o.status, o.lang, o.country_code, o.tenant_id % 3, o.deleted_at IS NULL)
ORDER BY g, 1 NULLS FIRST, 2 NULLS FIRST, 3 NULLS FIRST, 4 NULLS FIRST, 5 NULLS FIRST, 6 NULLS FIRST
