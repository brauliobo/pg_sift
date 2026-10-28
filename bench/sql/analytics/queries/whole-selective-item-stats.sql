-- whole-selective-item-stats: whole table, selective filter on a scalar path of attributes, a category and a price, three aggregates
-- sets: -
-- order: 2
-- key:
--@ pg
SELECT avg(i.price) AS avg_price, sum(i.price) AS sum_price, count(*) AS c
FROM orders o CROSS JOIN LATERAL jsonb_to_recordset(o.items) AS i(category text, price float8)
WHERE o.attributes->>'channel' = 'mobile' AND i.category = 'Electronics' AND i.price > 100
--@ flat
SELECT avg(i.price) AS avg_price, sum(i.price) AS sum_price, count(*) AS c
FROM pg_items i WHERE i.channel = 'mobile' AND i.category = 'Electronics' AND i.price > 100
--@ ch
SELECT avg(i.price) AS avg_price, sum(i.price) AS sum_price, count(*) AS c
FROM items_tuned i WHERE i.channel = 'mobile' AND i.category = 'Electronics' AND i.price > 100
