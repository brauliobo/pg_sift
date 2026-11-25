-- a LEFT JOIN written with the dimension first: the partitioned table is the nullable side
-- shape: part
-- expect: agg
-- key: -
-- scenario: yes
SELECT s.kind, count(t.id) AS n, sum(t.qty) AS q FROM tenants s LEFT JOIN t_part t ON s.id = t.grp WHERE s.id < 12 GROUP BY s.kind ORDER BY s.kind
