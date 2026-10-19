-- S5: top 10 (store, category) pairs by item count
-- shape: S5
-- expect: any
-- sets: K400 K4000
-- key: -
-- scenario: yes
SELECT o.store_id, i.category, count(*) AS items, avg(i.price) AS avg_price
FROM orders o CROSS JOIN LATERAL jsonb_to_recordset(o.items) AS i(category text, price float8)
WHERE o.store_id IN ({stores}) AND o.deleted_at IS NULL
GROUP BY o.store_id, i.category
ORDER BY items DESC, o.store_id, i.category
LIMIT 10
