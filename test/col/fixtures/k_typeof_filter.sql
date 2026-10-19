-- typeof: jsonb_typeof(path) in aggregate FILTERs, the whole table and by group
-- shape: quals
-- expect: agg
-- key: -
-- scenario: yes
SELECT store_id % 5 AS bucket, count(*) AS n,
       count(*) FILTER (WHERE jsonb_typeof(attributes->'weight') = 'number') AS numbers,
       count(*) FILTER (WHERE jsonb_typeof(attributes->'weight') = 'null') AS nulls,
       count(*) FILTER (WHERE jsonb_typeof(attributes->'weight') IS NULL) AS missing,
       sum(total) FILTER (WHERE jsonb_typeof(attributes->'tier') IN ('string', 'number') OR store_id < 100) AS revenue
FROM orders
GROUP BY 1
ORDER BY 1
