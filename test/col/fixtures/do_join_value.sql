-- DISTINCT ON whose select list holds a column of the joined table: the dimension's value of the first row of each group
-- shape: distinct_on
-- expect: agg
-- key: -
-- scenario: yes
SELECT DISTINCT ON (o.tenant_id) o.tenant_id, o.id, s.name, s.kind FROM orders o JOIN tenants s ON s.id = o.tenant_id ORDER BY o.tenant_id, o.placed_at DESC, o.id
