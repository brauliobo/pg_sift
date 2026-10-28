-- whole-top-category-kind: whole table, group by two low cardinality columns, top 10
-- sets: -
-- order: 3
-- key: 1 2
--@ pg
SELECT i.category, i.kind, count(*) AS c, avg(i.price) AS avg_price
FROM orders o CROSS JOIN LATERAL jsonb_to_recordset(o.items) AS i(category text, kind text, price float8)
GROUP BY 1, 2 ORDER BY c DESC, 1, 2 LIMIT 10
--@ flat
SELECT i.category, i.kind, count(*) AS c, avg(i.price) AS avg_price
FROM pg_items i GROUP BY 1, 2 ORDER BY c DESC, 1, 2 LIMIT 10
--@ ch
SELECT i.category AS category, i.kind AS kind, count(*) AS c, avg(i.price) AS avg_price
FROM items_tuned i GROUP BY category, kind ORDER BY c DESC, category, kind LIMIT 10
