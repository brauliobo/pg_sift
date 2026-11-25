-- a condition without a column (a stable function) is PostgreSQL's one-time filter: true keeps the rows, false gives no group
-- shape: flat
-- expect: agg
-- key: -
-- scenario: yes
SELECT status, count(*) AS n, sum(total) AS revenue FROM orders
WHERE tenant_id < 120 AND deleted_at IS NULL AND now() > '2020-01-01'::timestamptz AND current_setting('server_version_num')::int > 0
GROUP BY status ORDER BY status NULLS FIRST
