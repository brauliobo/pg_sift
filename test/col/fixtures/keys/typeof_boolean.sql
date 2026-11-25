-- typeof: a boolean leaf with a key that is missing in some orders, two type conjuncts on two leaves
-- shape: quals
-- expect: agg
-- key: -
-- scenario: yes
SELECT (attributes->>'normalized')::bool AS normalized, count(*) AS n, sum((attributes->>'weight')::float8) AS total
FROM orders
WHERE jsonb_typeof(attributes->'normalized') = 'boolean' AND jsonb_typeof(attributes->'weight') = 'number'
GROUP BY 1
ORDER BY 1
