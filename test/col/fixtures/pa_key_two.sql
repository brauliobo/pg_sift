-- two group keys, one of the partition key's table and the text column
-- shape: part
-- expect: agg
-- key: -
-- scenario: yes
SELECT grp, tag, count(*) AS n, sum(amount) AS revenue FROM t_part WHERE d >= DATE '2024-04-15' GROUP BY grp, tag ORDER BY grp, tag
