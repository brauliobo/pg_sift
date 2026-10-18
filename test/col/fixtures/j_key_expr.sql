-- join keys that are expressions of the leaves: the table is probed with the value PostgreSQL computes (lower() of a category, a sum of an integer)
-- shape: join
-- expect: agg
-- key: -
-- scenario: yes
SELECT cm.mapped_category, s.kind, count(*) AS n, count(DISTINCT o.id) AS orders, sum(i.qty) AS units
FROM orders o CROSS JOIN LATERAL jsonb_to_recordset(o.items) AS i(category text, kind text, qty int, price float8, discount float8)
JOIN categories_map cm ON cm.category = lower(i.category)
LEFT JOIN stores s ON s.id = o.store_id + i.qty - 3
WHERE o.store_id < 120 AND o.deleted_at IS NULL
GROUP BY cm.mapped_category, s.kind ORDER BY 1 NULLS FIRST, 2 NULLS FIRST
