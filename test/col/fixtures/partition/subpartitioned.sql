-- a partitioned partition read as the table: its leaves
-- shape: part
-- expect: agg
-- key: -
-- scenario: yes
SELECT grp, count(*) AS n, sum(qty) AS q FROM t_part_q3 GROUP BY grp ORDER BY grp
