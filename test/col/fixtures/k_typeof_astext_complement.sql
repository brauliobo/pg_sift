-- typeof: the complement, the JSON null and the missing key of an astext leaf
-- shape: quals
-- expect: agg
-- key: -
-- scenario: yes
SELECT count(*) AS n, count(*) FILTER (WHERE tenant_id < 50) AS few_stores,
       sum(total) FILTER (WHERE jsonb_typeof(attributes->'code') IS NULL) AS missing_total,
       sum(total) FILTER (WHERE jsonb_typeof(attributes->'code') = 'null') AS null_total
FROM orders
WHERE jsonb_typeof(attributes->'code') <> 'string' AND jsonb_typeof(attributes->'code') NOT IN ('object', 'array')
