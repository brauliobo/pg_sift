-- typeof: forms the scan does not take (an OR) keep the plan PostgreSQL has
-- shape: quals
-- expect: plain
-- key: -
-- scenario: yes
SELECT count(*) AS n
FROM orders
WHERE jsonb_typeof(attributes->'weight') = 'number' OR jsonb_typeof(attributes->'tier') = 'string'
