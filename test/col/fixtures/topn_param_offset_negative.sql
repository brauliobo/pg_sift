-- OFFSET $2 of a generic plan: a negative offset is the error of the Limit node
-- shape: topn_param
-- expect: any
-- key: -
-- scenario: yes
-- guc: plan_cache_mode = force_generic_plan
-- error: yes
-- prepare: (bigint, bigint)
-- args: 10, -1
SELECT id, tenant_id, total FROM orders WHERE memo @@> 'clean OR wire' ORDER BY placed_at DESC, id DESC LIMIT $1 OFFSET $2
