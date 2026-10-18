-- trap: sum and avg of float4 (PostgreSQL adds in float4 for sum and in float8 for avg): float4_pl over the decoded values (generic aggregate), in heap order
-- shape: trap
-- expect: agg
-- guc: seq_page_cost = 10000
-- guc: random_page_cost = 10000
-- key: -
SELECT grp, sum(f4) AS s, avg(f4) AS a, min(f4) AS lo, max(f4) AS hi FROM t_float WHERE f4 BETWEEN -1e30 AND 1e30 GROUP BY grp ORDER BY grp
