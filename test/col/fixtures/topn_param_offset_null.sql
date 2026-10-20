-- OFFSET $2 NULL of a generic plan is no offset
-- shape: topn_param
-- expect: agg
-- key: -
-- scenario: yes
-- guc: plan_cache_mode = force_generic_plan
-- prepare: (bigint, bigint)
-- args: 10, NULL
SELECT id, tenant_id, total FROM orders WHERE memo @@> 'clean OR wire' ORDER BY placed_at DESC, id DESC LIMIT $1 OFFSET $2
