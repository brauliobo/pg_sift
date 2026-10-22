-- the table with the index is not the first of the FROM list: the partitioned table is joined to by the dimension written first
-- shape: part
-- expect: agg
-- key: -
-- scenario: yes
SELECT s.kind, t.tag, count(*) AS n, sum(t.amount) AS revenue FROM stores s JOIN t_part t ON s.id = t.grp GROUP BY s.kind, t.tag ORDER BY s.kind, t.tag
