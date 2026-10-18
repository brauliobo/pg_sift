-- numeric group keys: the scaled value is the group, the key prints with the column's scale (trailing zeros), NULL is a group, two keys and a filter
-- shape: S6
-- expect: agg
-- key: -
-- scenario: yes
SELECT n2, grp, count(*) AS n, sum(n6) AS s6, min(n0) AS lo, max(id) AS hi
FROM t_num
WHERE n6 <> 0.000001
GROUP BY n2, grp
ORDER BY n2 NULLS FIRST, 2
