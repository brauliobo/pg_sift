-- count(DISTINCT) of a dense integer domain with negative values and NULLs: a bitmap per group, filtered, with FILTER and a HAVING above
-- shape: distinct
-- expect: agg
-- key: -
-- scenario: yes
SELECT grp, count(DISTINCT v) AS d, count(DISTINCT v) FILTER (WHERE x < 500000) AS half, count(*) AS n
FROM t_dist WHERE id > 100 GROUP BY grp HAVING count(DISTINCT v) > 0 ORDER BY grp
