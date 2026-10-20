-- NOT IN of a sub-select that unnests an array with a NULL, and without it
-- shape: filter
-- expect: agg
-- key: -
-- scenario: yes
SELECT o.status, count(*) AS n
FROM orders o
WHERE o.deleted_at IS NULL AND (o.customer_id NOT IN (SELECT unnest(ARRAY[1, 2, 3, 4, 5, 6, 7, NULL::int])) OR o.tenant_id NOT IN (SELECT unnest(ARRAY[1, 2, 3, 4, 5, 6, 7])))
GROUP BY o.status ORDER BY o.status NULLS FIRST
