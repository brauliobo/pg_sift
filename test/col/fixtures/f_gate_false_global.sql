-- a one-time filter that is false: no row reaches the aggregate, a query without GROUP BY still has its row
-- shape: flat
-- expect: agg
-- key: -
SELECT count(*) AS n, sum(total) AS revenue, min(placed_at) AS first, count(DISTINCT status) AS statuses FROM orders
WHERE tenant_id < 120 AND now() < '2020-01-01'::timestamptz
