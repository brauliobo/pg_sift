-- a RIGHT JOIN that keeps the rows of a partitioned table, with a condition of its own in the ON clause
-- shape: join
-- expect: agg
-- key: -
-- scenario: yes
SELECT p.grp, count(o.id) AS n, count(*) AS tuples FROM orders o RIGHT JOIN t_part_ni p ON p.id = o.tenant_id AND p.qty > 10 GROUP BY p.grp ORDER BY p.grp NULLS FIRST
