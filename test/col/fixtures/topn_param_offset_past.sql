-- LIMIT $1 OFFSET $2 of a generic plan: the rows wanted pass the cap by the offset
-- shape: topn_param
-- expect: agg
-- key: -
-- scenario: yes
-- guc: plan_cache_mode = force_generic_plan
-- prepare: (bigint, bigint)
-- args: 10, 100000
SELECT id, store_id, total FROM orders WHERE memo @@> 'clean OR wifi' ORDER BY placed_at DESC, id DESC LIMIT $1 OFFSET $2
