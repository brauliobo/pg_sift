-- ORDER BY a money column: the count of the smallest unit orders as money does, the largest and the smallest value, NULLs and equal values included
-- shape: topn
-- expect: agg
-- key: -
-- scenario: yes
SELECT id, m FROM t_wide ORDER BY m DESC NULLS LAST, id LIMIT 7 OFFSET 2
