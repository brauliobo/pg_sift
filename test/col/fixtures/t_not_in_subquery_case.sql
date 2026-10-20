-- a NOT IN sub-select inside a CASE that is a group key, and in a FILTER of an aggregate
-- shape: filter
-- expect: agg
-- key: -
-- scenario: yes
SELECT CASE WHEN o.customer_id NOT IN (SELECT c.customer_id FROM dim_customer c WHERE c.customer_id < 100 AND c.customer_id IS NOT NULL) THEN 'other' ELSE 'known' END AS grp,
       count(*) AS n, count(*) FILTER (WHERE o.store_id NOT IN (SELECT s.id FROM stores s WHERE s.kind = 'outlet')) AS not_outlet
FROM orders o
WHERE o.deleted_at IS NULL
GROUP BY 1 ORDER BY 1
