-- the integer cast of a metric (a member of an object leaf held as float8): the cast of the jsonb its stored float stands for, as a filter, a group key and an aggregate filter
-- shape: flat
-- expect: agg
-- key: 1
-- scenario: yes
SELECT (metrics -> 'p_total')::int AS score, count(*) AS orders, count(*) FILTER (WHERE (metrics -> 'quality')::bigint >= 4) AS good
FROM orders
WHERE store_id < 120 AND deleted_at IS NULL AND CAST(jsonb_extract_path(metrics, 'p_total') AS integer) >= 3
GROUP BY 1 ORDER BY 1
