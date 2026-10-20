-- the arms are parameters of a generic plan
-- shape: topn_or
-- expect: agg
-- key: -
-- scenario: yes
-- guc: plan_cache_mode = force_generic_plan
-- prepare: (text, text)
-- args: 'clean', 'wifi'
SELECT id, store_id, placed_at FROM orders WHERE memo @@> $1 OR memo @@> $2 ORDER BY placed_at DESC, id DESC LIMIT 10
