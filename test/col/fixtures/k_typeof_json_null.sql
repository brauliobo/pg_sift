-- typeof: a JSON null is the type 'null' and a missing key is none: jsonb_typeof(path) = 'null' keeps the first and never the second
-- shape: quals
-- expect: agg
-- key: -
-- scenario: yes
SELECT status, count(*) AS n, count(*) FILTER (WHERE tenant_id < 50) AS few_stores, sum(total) AS revenue
FROM orders
WHERE jsonb_typeof(attributes->'weight') = 'null' AND deleted_at IS NULL
GROUP BY status
ORDER BY status NULLS FIRST
