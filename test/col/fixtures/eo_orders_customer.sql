-- the shape on the orders: an item quantity against the store and the customer of the order (NULL for one order in ten)
-- shape: S7
-- expect: agg
-- sets: K4000
-- key: -
-- scenario: yes
SELECT o.lang, count(*) AS orders,
       count(*) FILTER (WHERE EXISTS (SELECT 1 FROM jsonb_to_recordset(o.items) AS i(category text, qty int) WHERE i.category = 'Toys' AND i.qty >= o.store_id)) AS toys_over_store,
       count(*) FILTER (WHERE EXISTS (SELECT 1 FROM jsonb_to_recordset(o.items) AS i(qty int) WHERE i.qty <= o.customer_id)) AS under_customer,
       count(*) FILTER (WHERE NOT EXISTS (SELECT 1 FROM jsonb_to_recordset(o.items) AS i(qty int) WHERE i.qty <> o.customer_id)) AS none_other_than_customer
FROM orders o
WHERE o.store_id IN ({stores}) AND o.deleted_at IS NULL
GROUP BY 1
ORDER BY 1
