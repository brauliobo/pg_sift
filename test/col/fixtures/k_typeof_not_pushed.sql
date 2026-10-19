-- typeof: forms the scan does not take (a JSON null, an inequality, a list of everything else, an OR) keep the plan PostgreSQL has
-- shape: quals
-- expect: plain
-- key: -
-- scenario: yes
SELECT count(*) FILTER (WHERE jsonb_typeof(attributes->'weight') = 'null') AS nulls,
       count(*) FILTER (WHERE jsonb_typeof(attributes->'weight') <> 'number') AS not_numbers,
       count(*) FILTER (WHERE jsonb_typeof(attributes->'weight') IN ('string', 'boolean', 'object', 'array')) AS others
FROM orders
WHERE jsonb_typeof(attributes->'weight') = 'number' OR jsonb_typeof(attributes->'tier') = 'string'
