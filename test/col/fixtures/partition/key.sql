-- the partition key itself is a group key
-- shape: part
-- expect: agg
-- key: -
-- scenario: yes
SELECT d, count(*) AS n, sum(qty) AS q FROM t_part WHERE d >= DATE '2024-03-20' AND d < DATE '2024-04-10' GROUP BY d ORDER BY d
