-- whole-month-language: whole table, group by month and language
-- sets: -
-- order: 6
-- key: 1 2
--@ pg
SELECT date_trunc('month', o.placed_at)::date AS month, o.lang, count(*) AS c, avg(i.price) AS avg_price
FROM orders o CROSS JOIN LATERAL jsonb_to_recordset(o.items) AS i(price float8)
GROUP BY 1, 2
--@ flat
SELECT date_trunc('month', i.placed_at)::date AS month, i.lang, count(*) AS c, avg(i.price) AS avg_price
FROM pg_items i GROUP BY 1, 2
--@ ch
SELECT toStartOfMonth(i.placed_at) AS month, i.lang AS lang, count(*) AS c, avg(i.price) AS avg_price
FROM items_tuned i GROUP BY month, lang
