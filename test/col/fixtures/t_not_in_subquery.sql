-- trap: NOT IN of a sub-select is NULL for a NULL on either side (it is not an anti join): the plan stays
-- shape: trap
-- expect: plain
-- key: -
SELECT o.status, count(*) AS n
FROM orders o
WHERE o.deleted_at IS NULL AND o.customer_id NOT IN (SELECT m.rank FROM map_vals m)
GROUP BY o.status ORDER BY o.status NULLS FIRST
