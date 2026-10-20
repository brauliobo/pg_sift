-- an OR of searches and a LIMIT that is a parameter
-- shape: topn_or
-- expect: agg
-- key: -
-- scenario: yes
-- guc: plan_cache_mode = force_generic_plan
-- prepare: (bigint)
-- args: NULL
SELECT id, tenant_id, placed_at FROM orders WHERE memo @@> 'clean' OR memo @@> 'wire' ORDER BY placed_at DESC, id DESC LIMIT $1
