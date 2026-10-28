-- whole-count-item-filter: whole table, count with one filter on an item column
-- sets: -
-- order: 1
-- key:
--@ pg
SELECT count(*) AS c
FROM orders o CROSS JOIN LATERAL jsonb_to_recordset(o.items) AS i(kind text)
WHERE i.kind = 'express'
--@ flat
SELECT count(*) AS c FROM pg_items i WHERE i.kind = 'express'
--@ ch
SELECT count(*) AS c FROM items_tuned i WHERE i.kind = 'express'
