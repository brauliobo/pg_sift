-- a Top-N with more sort keys than a heap holds sorts every row that passes, by the keys and the TID of the partition's heap
-- shape: topn
-- expect: agg
-- key: -
-- scenario: yes
SELECT id, grp, qty, d, amount FROM t_part ORDER BY grp, qty, d, amount, id LIMIT 20
