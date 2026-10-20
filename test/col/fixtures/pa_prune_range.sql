-- a range on the partition key that prunes the first and the last partitions: the live ones are read
-- shape: part
-- expect: agg
-- key: -
-- scenario: yes
SELECT tag, count(*) AS n, sum(amount) AS revenue
FROM t_part WHERE d >= DATE '2024-05-10' AND d < DATE '2024-08-20' GROUP BY tag ORDER BY tag
