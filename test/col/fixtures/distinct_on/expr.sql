-- DISTINCT ON an expression (a month) that the select list repeats, the biggest order of each month
-- shape: distinct_on
-- expect: agg
-- key: -
-- scenario: yes
SELECT DISTINCT ON (date_trunc('month', placed_at)) date_trunc('month', placed_at) AS month, id, total, tenant_id
FROM orders WHERE deleted_at IS NULL ORDER BY date_trunc('month', placed_at), total DESC NULLS LAST, id
