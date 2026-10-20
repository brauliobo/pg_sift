-- the sub-select reads the table the scan reads
-- shape: filter
-- expect: agg
-- key: -
-- scenario: yes
SELECT o.status, count(*) AS n
FROM orders o
WHERE o.deleted_at IS NULL AND o.customer_id NOT IN (SELECT p.customer_id FROM orders p WHERE p.store_id < 60 AND p.customer_id IS NOT NULL)
GROUP BY o.status ORDER BY o.status NULLS FIRST
