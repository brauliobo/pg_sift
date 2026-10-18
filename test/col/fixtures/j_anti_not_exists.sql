-- NOT EXISTS against a table with NULL keys: an item whose category has no row stays, and so does one whose category is NULL
-- shape: join
-- expect: agg
-- key: -
-- scenario: yes
SELECT i.category, count(*) AS n, count(DISTINCT o.id) AS orders, avg(i.price) AS price
FROM orders o CROSS JOIN LATERAL jsonb_to_recordset(o.items) AS i(category text, kind text, qty int, price float8, discount float8)
WHERE o.store_id < 120 AND o.deleted_at IS NULL AND NOT EXISTS (SELECT 1 FROM map_null m WHERE m.category = i.category)
GROUP BY i.category ORDER BY i.category NULLS FIRST
