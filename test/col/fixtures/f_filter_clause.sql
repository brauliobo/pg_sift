-- flat: FILTER clauses on every kind of qual
-- shape: flat
-- expect: agg
-- key: -
-- scenario: yes
SELECT status, count(*) AS n, count(*) FILTER (WHERE deleted_at IS NULL) AS live, count(*) FILTER (WHERE placed_at >= DATE '2025-01-01') AS recent,
       sum(tenant_id) FILTER (WHERE source LIKE 'marketplace%') AS marketplace_sum, avg(tenant_id) FILTER (WHERE country_code IN ('US', 'DE')) AS us_de_avg,
       max(placed_at) FILTER (WHERE lang <> 'en') AS last_foreign
FROM orders
GROUP BY status
ORDER BY status NULLS FIRST
