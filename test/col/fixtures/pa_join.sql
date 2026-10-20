-- an inner join of a partitioned table to a dimension table
-- shape: part
-- expect: agg
-- key: -
-- scenario: yes
SELECT s.kind, t.tag, count(*) AS n, sum(t.amount) AS revenue
FROM t_part t JOIN tenants s ON s.id = t.grp
GROUP BY s.kind, t.tag ORDER BY s.kind, t.tag
