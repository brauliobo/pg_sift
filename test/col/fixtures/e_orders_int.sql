-- integer, date and float8 expressions PostgreSQL evaluates over the orders: a modulo and a month as keys, quotients, a float8 root and a count of distinct remainders
-- shape: expr
-- expect: agg
-- key: -
-- scenario: yes
SELECT store_id % 7 AS k, date_trunc('month', placed_at) AS m, count(*) AS n, max(store_id / 3) AS q, min(store_id % 100) AS r, max(sqrt(store_id::float8)) AS s, count(DISTINCT store_id % 10) AS d
FROM orders WHERE store_id <> 0 AND 1000 / store_id > 1 GROUP BY 1, 2 ORDER BY 1, 2
