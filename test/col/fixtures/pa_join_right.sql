-- a RIGHT JOIN to a table that keeps its rows (a store no row of the partitioned table has is a NULL group), the partitioned table is the nullable side
-- shape: part
-- expect: agg
-- key: -
-- scenario: yes
SELECT s.kind, count(t.id) AS n, sum(t.qty) AS q, count(*) AS tuples FROM t_part t RIGHT JOIN tenants s ON s.id = t.grp WHERE s.id < 9 GROUP BY s.kind ORDER BY s.kind
