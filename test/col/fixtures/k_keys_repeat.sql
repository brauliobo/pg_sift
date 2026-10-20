-- the same column and the same expression named twice among six group columns: PostgreSQL keeps one key of each
-- shape: flat
-- expect: agg
-- key: -
-- scenario: yes
SELECT o.source, o.status, o.lang, o.country_code, o.tenant_id % 4 AS st4, o.source AS source_again, o.tenant_id % 4 AS st4_again, o.placed_at >= date '2024-01-01' AS recent,
       count(*) AS n, sum(o.total) AS revenue
FROM orders o
WHERE o.tenant_id < 200
GROUP BY o.source, o.status, o.lang, o.country_code, o.tenant_id % 4, o.source, o.tenant_id % 4, o.placed_at >= date '2024-01-01'
ORDER BY 1, 2 NULLS FIRST, 3, 4 NULLS FIRST, 5, 8
