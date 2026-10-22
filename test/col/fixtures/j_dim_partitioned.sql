-- an inner join to a partitioned table without an index: the plan of the table is read once when the scan starts
-- shape: join
-- expect: agg
-- key: -
-- scenario: yes
SELECT p.grp, count(*) AS n, sum(o.total) AS revenue FROM orders o JOIN t_part_ni p ON p.id = o.tenant_id GROUP BY p.grp ORDER BY p.grp
