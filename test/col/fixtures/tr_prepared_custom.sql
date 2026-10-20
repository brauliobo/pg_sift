-- ctid: the bounds of a prepared statement planned with its values (a custom plan) are constants
-- shape: tid
-- expect: agg
-- key: -
-- scenario: yes
-- guc: enable_tidscan = off
-- guc: plan_cache_mode = force_custom_plan
-- prepare: (tid, tid)
-- args: '(40,1)'::tid, '(350,1)'::tid
SELECT source, count(*) AS n, sum(store_id) AS s FROM orders WHERE ctid >= $1 AND ctid < $2 GROUP BY source ORDER BY source
