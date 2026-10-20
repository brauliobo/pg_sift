-- LIMIT $1 NULL of a generic plan, with a search: all the matches sorted
-- shape: topn_param
-- expect: agg
-- key: -
-- scenario: yes
-- guc: plan_cache_mode = force_generic_plan
-- prepare: (bigint)
-- args: NULL
SELECT id, tenant_id, total FROM orders WHERE memo @@> 'clean OR wire' ORDER BY placed_at DESC, id DESC LIMIT $1
