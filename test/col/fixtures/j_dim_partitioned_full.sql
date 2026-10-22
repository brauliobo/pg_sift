-- a FULL JOIN of the scanned table to a partitioned table: the rows of each side that match none
-- shape: join
-- expect: agg
-- key: -
-- scenario: yes
SELECT p.grp, count(o.id) AS n, count(p.id) AS dims, count(*) AS tuples FROM orders o FULL JOIN t_part_ni p ON p.id = o.tenant_id AND p.qty > 10 GROUP BY p.grp ORDER BY p.grp NULLS FIRST
