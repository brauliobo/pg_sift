-- trap: the aggregated table on the nullable side of an outer join
-- shape: trap
-- expect: any
-- key: -
SELECT s.kind, count(o.id) AS orders, count(*) AS rows, sum(o.store_id) AS store_sum
FROM stores s LEFT JOIN orders o ON o.store_id = s.id AND o.deleted_at IS NULL GROUP BY s.kind ORDER BY s.kind
