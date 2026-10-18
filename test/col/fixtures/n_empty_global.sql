-- NULL and empty sets: a global aggregate over no row is one row (NULL, 0)
-- shape: empty
-- expect: agg
-- key: -
SELECT count(*) AS n, count(store_id) AS c, sum(store_id) AS s, avg(store_id) AS a, min(placed_at) AS lo, max(source) AS hi, count(DISTINCT country_code) AS d
FROM orders
WHERE store_id < 0
