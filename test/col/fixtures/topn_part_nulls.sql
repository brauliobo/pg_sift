-- a Top-N over the partitions with two keys of NULLs in both orders and an OFFSET
-- shape: topn
-- expect: agg
-- key: -
-- scenario: yes
SELECT id, qty, amount FROM t_part WHERE grp < 4 ORDER BY qty DESC NULLS LAST, amount NULLS FIRST, id LIMIT 12 OFFSET 7
