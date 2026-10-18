-- a VARIADIC user aggregate (the arguments are an array expression) and an aggregate of six arguments
-- shape: flat
-- expect: agg
-- key: -
SELECT source, coldiff.sum_all(store_id, id % 5, 3) AS v, coldiff.sum_all(VARIADIC ARRAY[store_id, 1]) AS arr,
       coldiff.sum_six(store_id, id % 7, 1, 2, 3, id % 11) FILTER (WHERE status IS NOT NULL) AS six
FROM orders GROUP BY source ORDER BY source
