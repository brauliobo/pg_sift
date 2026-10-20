-- a leaf partition read directly: its own index, no wrapper
-- shape: part
-- expect: agg
-- key: -
-- scenario: yes
SELECT grp, count(*) AS n, sum(qty) AS q FROM t_part_q2 GROUP BY grp ORDER BY grp
