-- IN of a sub-select under OR is a hashed SubPlan, not a semi join: NULL customers are not in the sub-select and the other arm decides
-- shape: filter
-- expect: agg
-- key: -
-- scenario: yes
SELECT o.status, count(*) AS n
FROM orders o
WHERE o.deleted_at IS NULL AND (o.customer_id IN (SELECT c.customer_id FROM dim_customer c WHERE c.segment = 'vip') OR o.status = 'refunded')
GROUP BY o.status ORDER BY o.status NULLS FIRST
