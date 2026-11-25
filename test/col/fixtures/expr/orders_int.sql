-- integer, date and float8 expressions PostgreSQL evaluates over the orders: a modulo and a month as keys, quotients, a float8 root and a count of distinct remainders
-- shape: expr
-- expect: agg
-- key: -
-- scenario: yes
SELECT tenant_id % 7 AS k, date_trunc('month', placed_at) AS m, count(*) AS n, max(tenant_id / 3) AS q, min(tenant_id % 100) AS r, max(sqrt(tenant_id::float8)) AS s, count(DISTINCT tenant_id % 10) AS d
FROM orders WHERE tenant_id <> 0 AND 1000 / tenant_id > 1 GROUP BY 1, 2 ORDER BY 1, 2
