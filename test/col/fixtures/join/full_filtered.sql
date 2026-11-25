-- a FULL JOIN to a sub-select with a condition: the rows it leaves out are no rows of the join (they match nothing and are not kept)
-- shape: join
-- expect: agg
-- key: -
-- scenario: yes
SELECT s.kind, count(o.id) AS n, count(s.id) AS tenants, count(*) AS tuples FROM orders o FULL JOIN (SELECT id, kind FROM tenants WHERE id < 40) s ON s.id = o.tenant_id GROUP BY s.kind ORDER BY s.kind NULLS FIRST
