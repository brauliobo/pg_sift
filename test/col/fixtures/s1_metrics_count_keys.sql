-- S1: keys of the metrics with their counts and a HAVING on the count of distinct orders
-- shape: S1
-- expect: agg
-- sets: K10 K4000
-- key: -
SELECT kv.key, count(*) AS n, count(DISTINCT o.id) AS orders, min((kv.value #>> '{}')::float8) AS lo, max((kv.value #>> '{}')::float8) AS hi
FROM orders o CROSS JOIN LATERAL jsonb_each(o.metrics) kv
WHERE o.store_id IN ({stores}) AND o.placed_at >= DATE '2023-01-01' AND o.deleted_at IS NULL
GROUP BY kv.key
HAVING count(DISTINCT o.id) > 1
ORDER BY kv.key
