-- typeof: jsonb_typeof(path) in a CASE, as an aggregate argument and as a group key
-- shape: quals
-- expect: agg
-- key: -
-- scenario: yes
SELECT CASE jsonb_typeof(attributes->'tier') WHEN 'string' THEN 'text' WHEN 'number' THEN 'num' WHEN 'null' THEN 'nil' ELSE 'other' END AS kind,
       count(*) AS n, sum(CASE WHEN jsonb_typeof(attributes->'weight') = 'number' THEN 1 ELSE 0 END) AS heavy_numbers,
       sum(CASE WHEN jsonb_typeof(attributes->'weight') IS NULL THEN total END) AS missing_total
FROM orders
GROUP BY 1
ORDER BY 1
