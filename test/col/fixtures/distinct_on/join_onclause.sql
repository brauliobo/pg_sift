-- DISTINCT ON a column of the joined table
-- shape: distinct_on
-- expect: agg
-- key: -
-- scenario: yes
SELECT DISTINCT ON (s.kind) s.kind, o.id, o.total FROM orders o JOIN tenants s ON s.id = o.tenant_id ORDER BY s.kind, o.total DESC NULLS LAST, o.id
