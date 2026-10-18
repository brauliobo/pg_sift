-- round, trunc, floor and ceil of numeric expressions: scales above, at and below the value's, a negative one, half values away from zero, negative values, in aggregates
-- shape: S6
-- expect: agg
-- guc: seq_page_cost = 10000
-- guc: random_page_cost = 10000
-- key: -
SELECT grp, sum(round(n2, 1)) AS r1, sum(round(n2)) AS r0, sum(round(n6, 3)) AS r3, sum(round(n2, 4)) AS r4, sum(round(n2, -1)) AS rm1, sum(trunc(n2, 1)) AS t1, sum(trunc(n6)) AS t0,
       sum(trunc(n2, -1)) AS tm1, sum(floor(n2)) AS fl, sum(ceil(n2)) AS ce, sum(ceiling(n6)) AS ce6, max(floor(n6 * 10)) AS fl10, min(round(n2 * n2, 1)) AS rsq, sum(round(-n2, 1)) AS rneg
FROM t_num
GROUP BY grp
ORDER BY grp
