-- FETCH FIRST $1 ROWS WITH TIES of a generic plan: the top-n gives every row that ties with the last one (many orders share a tenant)
-- shape: topn_param
-- expect: agg
-- key: 1
-- scenario: yes
-- guc: plan_cache_mode = force_generic_plan
-- prepare: (bigint)
-- args: 3
SELECT id, tenant_id, total FROM orders ORDER BY tenant_id FETCH FIRST $1 ROWS WITH TIES
