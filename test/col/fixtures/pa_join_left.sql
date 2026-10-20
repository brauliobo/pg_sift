-- a left join of a partitioned table to a dimension table that has no row for some keys
-- shape: part
-- expect: agg
-- key: -
-- scenario: yes
SELECT s.kind, count(*) AS n, sum(t.qty) AS q
FROM t_part t LEFT JOIN (SELECT id, kind FROM stores WHERE id IN (1, 3)) s ON s.id = t.grp
WHERE t.d >= DATE '2024-02-15' AND t.d < DATE '2024-09-15' GROUP BY s.kind ORDER BY s.kind NULLS FIRST
