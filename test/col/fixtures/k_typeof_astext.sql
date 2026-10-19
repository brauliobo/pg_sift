-- typeof: a leaf of the text of any scalar (astext) holds a string, a number and a boolean as text: the type of each value is the kind its chunk keeps
-- shape: quals
-- expect: agg
-- key: -
-- scenario: yes
SELECT status, count(*) AS n, sum((attributes->>'code')::numeric) AS total, count(*) FILTER (WHERE store_id < 50) AS few_stores
FROM orders
WHERE jsonb_typeof(attributes->'code') = 'number' AND deleted_at IS NULL
GROUP BY status
ORDER BY status NULLS FIRST
