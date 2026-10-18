-- trap: metrics that are not numbers (strings, booleans, objects) under a cast to float8: the error of PostgreSQL (22P02) in every plan
-- shape: trap
-- expect: any
-- error: yes
-- key: -
SELECT kv.key, avg((kv.value #>> '{}')::float8) AS a FROM orders_odd o CROSS JOIN LATERAL jsonb_each(o.metrics) kv GROUP BY 1 ORDER BY 1
