-- SELECT DISTINCT of expressions of the columns, with a numeric filter
-- shape: flat
-- expect: agg
-- key: -
-- scenario: yes
SELECT DISTINCT date_trunc('month', placed_at) AS month, lower(source) AS src, tenant_id % 3 AS bucket FROM orders WHERE total > 20 ORDER BY 1, 2, 3
