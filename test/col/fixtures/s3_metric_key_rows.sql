-- a key of the metrics at the row level: a filter, a FILTER clause and aggregates
-- shape: S3
-- expect: agg
-- key: -
-- scenario: yes
SELECT o.source, count(*) AS n, count(*) FILTER (WHERE (o.metrics->>'quality')::float8 >= 3) AS good, avg((o.metrics->>'overall')::float8) AS overall,
       max((o.metrics->>'value')::float8) AS best
FROM orders o
WHERE o.store_id < 120 AND o.deleted_at IS NULL AND (o.metrics->>'overall')::float8 IS NOT NULL
GROUP BY 1 ORDER BY 1
