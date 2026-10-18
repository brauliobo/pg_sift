-- count(DISTINCT id) of the orders next to the items they unnest to
-- shape: flat
-- expect: agg
-- key: -
-- scenario: yes
SELECT i.category, count(*) AS items, count(DISTINCT o.id) AS orders, count(DISTINCT o.id) FILTER (WHERE i.discount > 0) AS discounted
FROM orders o CROSS JOIN LATERAL jsonb_to_recordset(o.items) AS i(category text, kind text, qty int, price float8, discount float8)
WHERE o.store_id < 120 AND o.deleted_at IS NULL
GROUP BY i.category ORDER BY 1
