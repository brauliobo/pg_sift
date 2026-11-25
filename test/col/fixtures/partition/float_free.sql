-- an average and a sum over a column of the whole table with a condition on the partition key and a NULL test
-- shape: part
-- expect: agg
-- key: -
-- scenario: yes
SELECT sum(qty) AS q, avg(qty) AS a, count(*) AS n FROM t_part WHERE qty IS NOT NULL AND d BETWEEN DATE '2024-03-15' AND DATE '2024-07-15'
