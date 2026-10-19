-- typeof: the types of an astext leaf as a group key and in a FILTER, a JSON null and a missing key apart, a string that looks like a number a string
-- shape: quals
-- expect: agg
-- key: -
-- scenario: yes
SELECT jsonb_typeof(attributes->'code') AS code_type, count(*) AS n,
       count(*) FILTER (WHERE jsonb_typeof(attributes->'code') IN ('number', 'boolean') OR store_id < 20) AS scalars_or_few,
       count(DISTINCT status) AS statuses
FROM orders
GROUP BY 1
ORDER BY 1 NULLS FIRST
