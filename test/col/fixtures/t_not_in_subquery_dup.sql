-- NOT IN of a sub-select that holds each value twice
-- shape: filter
-- expect: agg
-- key: -
-- scenario: yes
SELECT o.source, count(*) AS n
FROM orders o
WHERE o.deleted_at IS NULL AND o.customer_id NOT IN (SELECT s.i FROM (SELECT generate_series(1, 300) AS i UNION ALL SELECT generate_series(1, 300) % 7 + 100) s)
GROUP BY o.source ORDER BY o.source
