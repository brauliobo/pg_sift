-- typeof: NOT IN a list of types, on a float8 and a text leaf, and as NOT of an equality
-- shape: quals
-- expect: agg
-- key: -
-- scenario: yes
SELECT count(*) AS n, count(*) FILTER (WHERE store_id < 50) AS few_stores,
       sum(total) AS revenue
FROM orders
WHERE jsonb_typeof(attributes->'weight') NOT IN ('number', 'string') AND NOT (jsonb_typeof(attributes->'tier') = 'boolean')
