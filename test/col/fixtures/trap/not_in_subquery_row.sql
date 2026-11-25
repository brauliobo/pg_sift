-- a row-valued NOT IN reads two columns: the scan asks the sub-plan for each tuple
-- shape: agg
-- expect: agg
-- key: -
SELECT o.status, count(*) AS n
FROM orders o
WHERE o.deleted_at IS NULL AND (o.customer_id, o.tenant_id) NOT IN (SELECT c.customer_id, 7 FROM dim_customer c WHERE c.customer_id IS NOT NULL)
GROUP BY o.status ORDER BY o.status NULLS FIRST
