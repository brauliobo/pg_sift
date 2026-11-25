-- typeof: jsonb_typeof(path) as a group key: the five types, 'null' for a JSON null and a NULL group for a missing key, on two leaves
-- shape: quals
-- expect: agg
-- key: -
-- scenario: yes
SELECT jsonb_typeof(attributes->'weight') AS weight_type, jsonb_typeof(attributes->'tier') AS tier_type, count(*) AS n,
       count(*) FILTER (WHERE tenant_id < 50) AS few_stores, sum(total) AS revenue
FROM orders
WHERE deleted_at IS NULL
GROUP BY 1, 2
ORDER BY 1 NULLS FIRST, 2 NULLS FIRST
