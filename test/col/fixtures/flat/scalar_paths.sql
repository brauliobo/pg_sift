-- flat: scalar paths of the attributes object (->>) as group keys and a metric path as the aggregate
-- shape: flat
-- expect: any
-- key: -
-- scenario: yes
SELECT attributes->>'channel' AS channel, attributes->>'normalized' AS normalized, count(*) AS n, avg((metrics->>'m_total')::float8) AS m_total
FROM orders
WHERE deleted_at IS NULL
GROUP BY 1, 2
ORDER BY 1, 2 NULLS FIRST
