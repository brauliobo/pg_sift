-- typeof: the complement of a type (<> and NOT IN): every type of the path but the one, and none (a missing key has no type, PostgreSQL drops it)
-- shape: quals
-- expect: agg
-- key: -
-- scenario: yes
SELECT status, count(*) AS n, count(*) FILTER (WHERE tenant_id < 50) AS few_stores
FROM orders
WHERE jsonb_typeof(attributes->'weight') <> 'number' AND deleted_at IS NULL
GROUP BY status
ORDER BY status NULLS FIRST
