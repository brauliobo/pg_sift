-- LIMIT $1 OFFSET $2 of a generic plan
-- shape: topn_param
-- expect: agg
-- key: -
-- scenario: yes
-- guc: plan_cache_mode = force_generic_plan
-- prepare: (bigint, bigint)
-- args: 10, 5
SELECT id, tenant_id, total FROM orders WHERE memo @@> 'cable OR wire' ORDER BY placed_at DESC, id DESC LIMIT $1 OFFSET $2
