-- the same with a condition in the ON clause as well
-- shape: join
-- expect: agg
-- key: -
-- scenario: yes
SELECT s.kind, count(o.id) AS n, count(*) AS tuples FROM orders o FULL JOIN (SELECT id, kind FROM stores WHERE id % 2 = 0) s ON s.id = o.store_id AND o.total > 200 GROUP BY s.kind ORDER BY s.kind NULLS FIRST
