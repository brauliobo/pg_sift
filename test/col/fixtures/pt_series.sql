-- generate_series as a dimension: a join to a function
-- shape: join
-- expect: agg
-- key: -
-- scenario: yes
SELECT g.n % 3 AS bucket, count(*) AS n, sum(o.total) AS revenue
FROM orders o JOIN generate_series(1, 30) AS g(n) ON o.tenant_id = g.n
WHERE o.deleted_at IS NULL
GROUP BY 1 ORDER BY 1
