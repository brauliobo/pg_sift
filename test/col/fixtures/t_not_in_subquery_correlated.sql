-- trap: the sub-select reads a column of the outer row (a different answer for every row): the plan stays
-- shape: trap
-- expect: plain
-- key: -
SELECT o.status, count(*) AS n
FROM orders o
WHERE o.deleted_at IS NULL AND o.customer_id NOT IN (SELECT c.customer_id FROM dim_customer c WHERE c.customer_id IS NOT NULL AND c.customer_id % 5 = o.store_id % 5)
GROUP BY o.status ORDER BY o.status NULLS FIRST
