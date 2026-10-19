-- typeof: jsonb_typeof(path) in a branch of an OR beside predicates of other columns and of another path (NULL semantics kept: a missing key is not <>)
-- shape: quals
-- expect: agg
-- key: -
-- scenario: yes
SELECT status, count(*) AS n, sum(total) AS revenue
FROM orders
WHERE jsonb_typeof(attributes->'weight') = 'null' OR (jsonb_typeof(attributes->'tier') <> 'string' AND tenant_id < 100) OR jsonb_typeof(attributes->'tier') IS NULL
GROUP BY status
ORDER BY status NULLS FIRST
