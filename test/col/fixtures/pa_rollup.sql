-- a rollup over a partitioned table
-- shape: part
-- expect: agg
-- key: -
-- scenario: yes
SELECT grp, tag, count(*) AS n, sum(qty) AS q FROM t_part GROUP BY ROLLUP (grp, tag) ORDER BY grp NULLS LAST, tag NULLS LAST
