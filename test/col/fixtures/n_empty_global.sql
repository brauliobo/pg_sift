-- NULL and empty sets: a global aggregate over no row is one row (NULL, 0)
-- shape: empty
-- expect: agg
-- key: -
SELECT count(*) AS n, count(tenant_id) AS c, sum(tenant_id) AS s, avg(tenant_id) AS a, min(placed_at) AS lo, max(source) AS hi, count(DISTINCT country_code) AS d
FROM orders
WHERE tenant_id < 0
