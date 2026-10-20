-- ROLLUP of seven columns, the limit (the set number is the eighth key)
-- shape: sets
-- expect: agg
-- sets: K400
-- key: -
-- scenario: yes
SELECT o.source, o.status, o.lang, o.country_code, o.tenant_id % 3 AS st3, o.deleted_at IS NULL AS live, o.customer_id IS NULL AS anon,
       grouping(o.source, o.status, o.lang, o.country_code, o.tenant_id % 3, o.deleted_at IS NULL, o.customer_id IS NULL) AS g, count(*) AS n, sum(o.total) AS revenue
FROM orders o
WHERE o.tenant_id IN ({tenants})
GROUP BY ROLLUP (o.source, o.status, o.lang, o.country_code, o.tenant_id % 3, o.deleted_at IS NULL, o.customer_id IS NULL)
ORDER BY g, 1 NULLS FIRST, 2 NULLS FIRST, 3 NULLS FIRST, 4 NULLS FIRST, 5 NULLS FIRST, 6 NULLS FIRST, 7 NULLS FIRST
