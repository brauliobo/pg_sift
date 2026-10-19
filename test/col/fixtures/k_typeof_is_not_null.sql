-- typeof: IS NOT NULL (every type but none), with the values of the leaf read for the numbers and the JSON nulls
-- shape: quals
-- expect: agg
-- key: -
-- scenario: yes
SELECT tenant_id % 7 AS bucket, count(*) AS n, sum((attributes->>'weight')::float8) AS total
FROM orders
WHERE jsonb_typeof(attributes->'weight') IN ('number', 'null') AND NOT (jsonb_typeof(attributes->'tier') IS NULL)
GROUP BY 1
ORDER BY 1
