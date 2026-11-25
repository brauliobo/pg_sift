-- DISTINCT ON over a left join: the NULL of a row without a match is a value like another
-- shape: distinct_on
-- expect: agg
-- key: -
-- scenario: yes
SELECT DISTINCT ON (o.tenant_id) o.tenant_id, o.id, v.weight FROM orders o LEFT JOIN map_vals v ON v.category = o.source ORDER BY o.tenant_id, v.weight DESC NULLS LAST, o.id
