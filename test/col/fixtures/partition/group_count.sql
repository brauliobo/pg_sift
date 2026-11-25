-- a partitioned table: count, sum and min/max per group over every partition (one has other attribute numbers, one is partitioned again)
-- shape: part
-- expect: agg
-- key: -
-- scenario: yes
SELECT grp, count(*) AS n, count(qty) AS q, sum(qty) AS s, min(d) AS first_day, max(d) AS last_day
FROM t_part GROUP BY grp ORDER BY grp
