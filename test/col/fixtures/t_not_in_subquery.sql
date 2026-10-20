-- NOT IN of a sub-select with a NULL: it is never true (NULL for a value the sub-select does not hold), so no row passes. PostgreSQL's hashed SubPlan applies the rule, the scan caches its answer per customer
-- shape: filter
-- expect: agg
-- key: -
-- scenario: yes
SELECT o.status, count(*) AS n
FROM orders o
WHERE o.deleted_at IS NULL AND o.customer_id NOT IN (SELECT m.rank FROM map_vals m)
GROUP BY o.status ORDER BY o.status NULLS FIRST
