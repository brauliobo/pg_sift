-- G5: whole table, group by a high cardinality column (the store), top 10
-- sets: -
-- key: 1
--@ pg
SELECT o.store_id, count(*) AS c
FROM orders o CROSS JOIN LATERAL jsonb_to_recordset(o.items) AS i(qty int)
GROUP BY 1 ORDER BY c DESC, 1 LIMIT 10
--@ flat
SELECT i.store_id, count(*) AS c FROM pg_items i GROUP BY 1 ORDER BY c DESC, 1 LIMIT 10
--@ ch
SELECT i.store_id AS store_id, count(*) AS c FROM items_tuned i GROUP BY store_id ORDER BY c DESC, store_id LIMIT 10
