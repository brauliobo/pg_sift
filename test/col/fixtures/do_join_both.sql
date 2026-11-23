-- DISTINCT ON with a joined column in the select list and a joined sort column
-- shape: distinct_on
-- expect: agg
-- key: -
-- scenario: yes
SELECT DISTINCT ON (o.status) o.status, o.id, s.kind FROM orders o JOIN tenants s ON s.id = o.tenant_id ORDER BY o.status, s.name, o.id
