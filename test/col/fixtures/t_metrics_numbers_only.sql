-- trap: the same documents with a filter on the JSON type: no error, an object with numbers of any magnitude (1e300)
-- shape: trap
-- expect: any
-- key: -
SELECT kv.key, count(*) AS n, sum((kv.value #>> '{}')::float8) AS s, max((kv.value #>> '{}')::float8) AS hi
FROM orders_odd o CROSS JOIN LATERAL jsonb_each(o.metrics) kv WHERE jsonb_typeof(kv.value) = 'number' GROUP BY 1 ORDER BY 1
