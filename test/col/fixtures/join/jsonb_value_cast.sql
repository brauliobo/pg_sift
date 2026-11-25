-- the value of jsonb_each cast to a number (not through its text), as a key's value to average and to compare
-- shape: flat
-- expect: agg
-- key: 1
-- scenario: yes
SELECT kv.key, avg(kv.value::float8) AS avg_value, count(*) FILTER (WHERE kv.value::float8 >= 4) AS high, count(DISTINCT o.id) AS orders
FROM orders o CROSS JOIN LATERAL jsonb_each(o.metrics) kv
WHERE o.tenant_id < 120 AND o.deleted_at IS NULL
GROUP BY kv.key ORDER BY 1
