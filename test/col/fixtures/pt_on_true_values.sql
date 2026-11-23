-- a cross join to a VALUES list with a repeated row: the row counts as often as it is there
-- shape: join
-- expect: agg
-- key: -
SELECT m.k, count(*) AS n, sum(o.total) AS revenue
FROM orders o CROSS JOIN (VALUES (1), (2), (2)) m(k)
WHERE o.deleted_at IS NULL
GROUP BY m.k ORDER BY m.k
