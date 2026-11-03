-- NOT EXISTS over a table that only filters is an ordered walk with the keys of the table as a NOT IN conjunct (agg_topn_plan_keys): the scan is a choice of the cost, which the rows the scenarios change move, so either plan is right and the rows are the reference's
-- shape: topn
-- expect: any
-- key: -
-- scenario: yes
SELECT id, tenant_id, total FROM orders o WHERE NOT EXISTS (SELECT 1 FROM tenants s WHERE s.id = o.tenant_id AND s.kind = 'outlet') ORDER BY total DESC, id LIMIT 10
