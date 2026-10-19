-- window: the top two sources of every status, by a row number over a partition of the groups
-- shape: flat
-- expect: agg
-- key: -
-- scenario: yes
SELECT * FROM (
  SELECT status, source, count(*) AS n, sum(total) AS revenue, row_number() OVER (PARTITION BY status ORDER BY count(*) DESC, source) AS rn
  FROM orders
  GROUP BY status, source
) t
WHERE rn <= 2
ORDER BY status NULLS FIRST, rn
