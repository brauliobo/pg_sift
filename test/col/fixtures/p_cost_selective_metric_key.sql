-- cost: a selective set of orders grouped by a metric of a free key object keeps the normal plan (the scan looks the key up in every row of the table)
-- shape: S3
-- expect: plain
-- key: -
SELECT (metrics->>'m_total')::float8 AS m_total, count(*) AS orders
FROM orders WHERE id < 40 AND deleted_at IS NULL
GROUP BY 1 ORDER BY 1
