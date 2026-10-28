-- store-top-categories: per-store rows, the top 3 item categories of each store by revenue, with their orders and customers
-- sets: K10 K400 K4000
-- order: 5
-- key: 1 2
--@ pg
SELECT tenant_id, category, revenue, orders, customers, rn
FROM (SELECT tenant_id, category, revenue, orders, customers, row_number() OVER (PARTITION BY tenant_id ORDER BY revenue DESC, category) AS rn
      FROM (SELECT o.tenant_id, i.category, sum(i.qty * (i.price - i.discount)) AS revenue, count(DISTINCT o.id) AS orders,
                   count(DISTINCT o.customer_id) AS customers
            FROM orders o
            CROSS JOIN LATERAL jsonb_to_recordset(o.items) AS i(category text, qty int, price float8, discount float8)
            WHERE o.tenant_id IN ({tenants}) AND o.source IN ({sources}) AND o.deleted_at IS NULL AND {range(o.placed_at)}
            GROUP BY o.tenant_id, i.category) g) r
WHERE rn <= 3
--@ flat
SELECT tenant_id, category, revenue, orders, customers, rn
FROM (SELECT tenant_id, category, revenue, orders, customers, row_number() OVER (PARTITION BY tenant_id ORDER BY revenue DESC, category) AS rn
      FROM (SELECT i.tenant_id, i.category, sum(i.qty * (i.price - i.discount)) AS revenue, count(DISTINCT i.order_id) AS orders,
                   count(DISTINCT i.customer_id) AS customers
            FROM pg_items i
            WHERE i.tenant_id IN ({tenants}) AND i.source IN ({sources}) AND i.deleted_at IS NULL AND {range(i.placed_at)}
            GROUP BY i.tenant_id, i.category) g) r
WHERE rn <= 3
--@ ch
SELECT tenant_id, category, revenue, orders, customers, rn
FROM (SELECT tenant_id, category, revenue, orders, customers, row_number() OVER (PARTITION BY tenant_id ORDER BY revenue DESC, category) AS rn
      FROM (SELECT i.tenant_id AS tenant_id, i.category AS category, sum(i.qty * (i.price - i.discount)) AS revenue, count(DISTINCT i.order_id) AS orders,
                   count(DISTINCT i.customer_id) AS customers
            FROM items_tuned i
            WHERE i.tenant_id IN ({tenants}) AND i.source IN ({sources}) AND i.deleted = 0 AND {range(i.placed_at)}
            GROUP BY tenant_id, category) g) r
WHERE rn <= 3
