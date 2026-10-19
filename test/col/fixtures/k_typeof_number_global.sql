-- typeof: the same without a group key, with a comparison on the leaf itself and the type given as the one member of a list
-- shape: quals
-- expect: agg
-- key: -
-- scenario: yes
SELECT count(*) AS n, sum((attributes->>'weight')::float8) AS total, avg((attributes->>'weight')::float8) AS mean
FROM orders
WHERE jsonb_typeof(attributes->'weight') IN ('number') AND (attributes->>'weight')::float8 > 40
