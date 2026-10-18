-- G5: whole table, group by a high cardinality column (the store), top 10
-- sets: -
-- key: 1
--@ pg
SELECT o.tenant_id, count(*) AS c
FROM orders o CROSS JOIN LATERAL jsonb_to_recordset(o.items) AS i(qty int)
GROUP BY 1 ORDER BY c DESC, 1 LIMIT 10
--@ flat
SELECT i.tenant_id, count(*) AS c FROM pg_items i GROUP BY 1 ORDER BY c DESC, 1 LIMIT 10
--@ ch
SELECT i.tenant_id AS tenant_id, count(*) AS c FROM items_tuned i GROUP BY tenant_id ORDER BY c DESC, tenant_id LIMIT 10
