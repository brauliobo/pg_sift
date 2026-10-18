-- trap: an inner join to an empty mapping table: no row, and the global aggregate still has its one row
-- shape: trap
-- expect: agg
-- key: -
-- scenario: yes
SELECT count(*) AS n, sum(i.qty) AS qty, count(DISTINCT o.id) AS orders
FROM orders o CROSS JOIN LATERAL jsonb_to_recordset(o.items) AS i(category text, kind text, qty int, price float8, discount float8)
JOIN map_empty m ON m.category = i.category
WHERE o.store_id < 120 AND o.deleted_at IS NULL
