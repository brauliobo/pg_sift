-- a Top-N of a partitioned table: the winners come from partitions that number their columns differently (a dropped column, another order), a hash sub-partition and the default one
-- shape: topn
-- expect: agg
-- key: -
-- scenario: yes
SELECT id, grp, d, qty, amount, tag, note FROM t_part ORDER BY d DESC, id LIMIT 25
