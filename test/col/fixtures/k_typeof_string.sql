-- typeof: the strings of a text leaf among numbers, booleans, objects, arrays, JSON nulls and missing keys
-- shape: quals
-- expect: agg
-- key: -
-- scenario: yes
SELECT attributes->>'tier' AS tier, count(*) AS n, count(DISTINCT customer_id) AS customers, max(placed_at) AS last_order
FROM orders
WHERE jsonb_typeof(attributes->'tier') = 'string' AND source LIKE 'marketplace%'
GROUP BY 1
ORDER BY 1
