-- typeof: the numbers of a float8 leaf among strings, booleans, objects, arrays, JSON nulls and missing keys (jsonb_typeof(path) = 'number')
-- shape: quals
-- expect: agg
-- key: -
-- scenario: yes
SELECT status, count(*) AS n, sum((attributes->>'weight')::float8) AS total, min((attributes->>'weight')::float8) AS lightest,
       max((attributes->>'weight')::float8) AS heaviest, count(*) FILTER (WHERE store_id < 50) AS few_stores
FROM orders
WHERE jsonb_typeof(attributes->'weight') = 'number' AND deleted_at IS NULL
GROUP BY status
ORDER BY status NULLS FIRST
