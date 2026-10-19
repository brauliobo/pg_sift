-- a generic aggregate with a FILTER, so some groups have no input in some workers: the partial states that are NULL merge as NULL
-- shape: flat
-- expect: agg
-- key: -
SELECT status, stddev_pop(store_id) FILTER (WHERE store_id % 97 = 0) AS sd, bool_or(deleted_at IS NULL) AS live, min(country_code) AS lo, max(lang) AS hi
FROM orders GROUP BY status ORDER BY status NULLS FIRST
