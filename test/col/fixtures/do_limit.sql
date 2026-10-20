-- DISTINCT ON under a top-level ORDER BY ... LIMIT of the groups, with an OFFSET
-- shape: distinct_on
-- expect: agg
-- key: -
-- scenario: yes
SELECT DISTINCT ON (tenant_id) tenant_id, id, placed_at FROM orders ORDER BY tenant_id, placed_at DESC, id LIMIT 7 OFFSET 2
