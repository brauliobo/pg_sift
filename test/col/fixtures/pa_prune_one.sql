-- a range inside one partition: the scan of one live partition
-- shape: part
-- expect: agg
-- key: -
-- scenario: yes
SELECT grp, count(*) AS n, sum(qty) AS q FROM t_part WHERE d >= DATE '2024-02-01' AND d < DATE '2024-03-01' GROUP BY grp ORDER BY grp
