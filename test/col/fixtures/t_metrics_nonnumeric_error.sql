-- trap: metrics that are not numbers (strings, booleans, objects) under a cast to float8: both plans fail; the scan reports a value the index cannot store (0A000), the normal plan the cast (22P02)
-- shape: trap
-- expect: any
-- error: either
-- key: -
SELECT kv.key, avg((kv.value #>> '{}')::float8) AS a FROM orders_odd o CROSS JOIN LATERAL jsonb_each(o.metrics) kv GROUP BY 1 ORDER BY 1
