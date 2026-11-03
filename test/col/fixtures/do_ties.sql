-- DISTINCT ON: the ORDER BY has no unique column, so rows of a store tie on the placed_at; the scan keeps the first one it meets (the select list holds the sort columns only, so the result is the same in every plan)
-- shape: distinct_on
-- expect: agg
-- key: -
SELECT DISTINCT ON (tenant_id) tenant_id, placed_at FROM orders WHERE tenant_id < 50 ORDER BY tenant_id, placed_at DESC
