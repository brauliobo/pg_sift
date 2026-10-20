-- trap: a volatile function in the sub-select (it may answer differently in the plan that evaluates it again): the plan stays
-- shape: trap
-- expect: plain
-- key: -
SELECT count(*) AS n
FROM orders o
WHERE o.deleted_at IS NULL AND o.customer_id NOT IN (SELECT c.customer_id FROM dim_customer c WHERE c.customer_id IS NOT NULL AND random() < 2)
