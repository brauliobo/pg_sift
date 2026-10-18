-- flat: GROUP BY expressions: a CASE, a boolean, functions of a varchar(2) column and integer arithmetic
-- shape: flat
-- expect: any
-- key: -
SELECT CASE WHEN store_id < 20 THEN 'small' WHEN store_id < 100 THEN 'medium' ELSE 'large' END AS bucket, (placed_at >= DATE '2024-01-01') AS recent,
       lower(country_code) AS cc, left(country_code, 1) AS initial, store_id % 7 AS m7, count(*) AS n, sum(store_id * 2 + 1) AS expr_sum
FROM orders
WHERE deleted_at IS NULL AND country_code < 'C'
GROUP BY 1, 2, 3, 4, 5
ORDER BY 1, 2, 3 NULLS FIRST, 4 NULLS FIRST, 5
