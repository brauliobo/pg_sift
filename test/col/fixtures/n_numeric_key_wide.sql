-- numeric group keys of 18 digits and six decimals next to an integer key, and a bigint key: many distinct values, negative values, NULL
-- shape: S6
-- expect: agg
-- guc: seq_page_cost = 10000
-- guc: random_page_cost = 10000
-- key: -
SELECT grp, n6, count(*) AS n, sum(n0) AS s0
FROM t_num
GROUP BY grp, n6
ORDER BY grp, n6 NULLS FIRST
