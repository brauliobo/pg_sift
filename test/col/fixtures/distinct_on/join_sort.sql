-- DISTINCT ON ordered by a column of the joined table, after the ON column
-- shape: distinct_on
-- expect: agg
-- key: -
-- scenario: yes
SELECT DISTINCT ON (o.source) o.source, o.id, o.total FROM orders o JOIN tenants s ON s.id = o.tenant_id ORDER BY o.source, s.name DESC, o.id
