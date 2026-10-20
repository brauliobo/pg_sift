-- six group columns over no rows: no group, not one group of NULLs
-- shape: flat
-- expect: agg
-- key: -
SELECT o.source, o.status, o.lang, o.country_code, o.store_id % 5 AS st5, o.placed_at > date '2023-06-30' AS late, count(*) AS n, sum(o.total) AS revenue
FROM orders o
WHERE o.store_id < 0
GROUP BY 1, 2, 3, 4, 5, 6
ORDER BY 1, 2, 3, 4, 5, 6
