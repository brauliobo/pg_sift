-- typeof: jsonb_typeof(path) IS NULL is a path with no value (a missing key, a NULL document), not a JSON null; IS NOT NULL is every type
-- shape: quals
-- expect: agg
-- key: -
-- scenario: yes
SELECT status, count(*) AS n, sum(total) AS revenue
FROM orders
WHERE jsonb_typeof(attributes->'weight') IS NULL
GROUP BY status
ORDER BY status NULLS FIRST
