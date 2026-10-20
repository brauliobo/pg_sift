-- a partitioned table: no group by, aggregates of every kind of column
-- shape: part
-- expect: agg
-- key: -
-- scenario: yes
SELECT count(*) AS n, count(qty) AS q, sum(amount) AS revenue, avg(qty) AS mean_qty, min(tag) AS first_tag, max(tag) AS last_tag, min(d) AS first_day
FROM t_part
