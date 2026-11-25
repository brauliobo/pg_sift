-- a branch partitioned by hash: a condition on its key keeps one leaf
-- shape: part
-- expect: agg
-- key: -
-- scenario: yes
SELECT count(*) AS n, sum(amount) AS revenue, max(d) AS last_day FROM t_part WHERE d >= DATE '2024-07-01' AND d < DATE '2024-10-01' AND id = 4
