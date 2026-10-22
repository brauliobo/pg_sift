-- a Top-N of the partitions a range of the partition key leaves, ordered by another column
-- shape: topn
-- expect: agg
-- key: -
-- scenario: yes
SELECT id, d, amount FROM t_part WHERE d >= DATE '2024-05-01' AND d < DATE '2024-11-01' ORDER BY amount DESC NULLS LAST, id LIMIT 10
