-- a FULL JOIN of a partitioned table to a table that keeps its rows: the rows of the partitioned table that match none (the ON clause leaves out stores 1 and 2) and the stores no row has
-- shape: part
-- expect: agg
-- key: -
-- scenario: yes
SELECT s.kind, count(t.id) AS n, sum(t.amount) AS revenue, count(*) AS tuples FROM t_part t FULL JOIN tenants s ON s.id = t.grp AND s.id > 2 GROUP BY s.kind ORDER BY s.kind NULLS FIRST
