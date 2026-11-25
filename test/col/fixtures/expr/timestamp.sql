-- timestamp expressions PostgreSQL evaluates: a month as a key, a formatted key next to a quotient of the numeric total, over the orders with every heap state of the scenarios
-- shape: expr
-- expect: agg
-- key: -
-- scenario: yes
SELECT date_trunc('month', deleted_at) AS m, to_char(deleted_at, 'YY-MM') AS tc, count(*) AS n, max(total * 2.5 / (id % 7 + 1)) AS q, sum(total / (tenant_id + 1)) FILTER (WHERE deleted_at IS NOT NULL) AS s
FROM orders GROUP BY 1, 2 ORDER BY 1 NULLS FIRST
