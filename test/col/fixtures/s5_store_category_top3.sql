-- S5: per-store rows, the top 3 categories of each store by revenue (a window over the aggregate)
-- shape: S5
-- expect: any
-- sets: K10 K400
-- key: -
SELECT store_id, category, revenue, orders, rn
FROM (SELECT store_id, category, revenue, orders, row_number() OVER (PARTITION BY store_id ORDER BY revenue DESC, category) AS rn
      FROM (SELECT o.store_id, i.category, sum(i.qty * (i.price - i.discount)) AS revenue, count(DISTINCT o.id) AS orders
            FROM orders o CROSS JOIN LATERAL jsonb_to_recordset(o.items) AS i(category text, qty int, price float8, discount float8)
            WHERE o.store_id IN ({stores}) AND o.deleted_at IS NULL AND {range(o.placed_at)}
            GROUP BY o.store_id, i.category) g) r
WHERE rn <= 3
ORDER BY store_id, rn
