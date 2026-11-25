-- ORDER BY a cidr column: the mask length orders after the address bits
-- shape: topn
-- expect: agg
-- key: -
SELECT id, cd FROM t_net ORDER BY cd, id LIMIT 12 OFFSET 1
