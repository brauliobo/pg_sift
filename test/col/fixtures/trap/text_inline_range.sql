-- flat: range operators on long text in the C collation, bytewise on the strings
-- shape: flat
-- expect: agg
-- key: -
-- scenario: yes
SELECT grp, count(*) AS n, min(id) AS lo FROM t_text WHERE t COLLATE "C" >= repeat('b', 300) AND t COLLATE "C" < repeat('c', 280) GROUP BY grp ORDER BY grp
