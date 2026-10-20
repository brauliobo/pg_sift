-- trap: <> ALL of an ARRAY sub-select reads the result of an initplan (a parameter of the query), not a hashed SubPlan: the plan stays
-- shape: trap
-- expect: plain
-- key: -
SELECT o.status, count(*) AS n
FROM orders o
WHERE o.deleted_at IS NULL AND o.store_id <> ALL (ARRAY(SELECT s.id FROM stores s WHERE s.id < 380))
GROUP BY o.status ORDER BY o.status NULLS FIRST
