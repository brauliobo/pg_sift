-- trap: a lookup join of a column of the order (no unnest): the replication of the orders
-- shape: trap
-- expect: agg
-- key: -
-- scenario: yes
SELECT m.tag, count(*) AS n, count(DISTINCT o.id) AS orders, sum(o.store_id) AS stores
FROM orders o LEFT JOIN map_dup m ON m.category = o.source
WHERE o.deleted_at IS NULL
GROUP BY m.tag ORDER BY 1 NULLS FIRST
