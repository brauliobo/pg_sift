-- conditions on columns that are not the partition key: every partition is read
-- shape: part
-- expect: agg
-- key: -
-- scenario: yes
SELECT grp, count(*) AS n, sum(amount) AS revenue FROM t_part WHERE qty > 20 AND tag <> 'plain' AND grp IN (1, 3, 4) GROUP BY grp ORDER BY grp
