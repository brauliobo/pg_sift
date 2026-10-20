-- NOT IN of an empty sub-select is true for every row, a NULL customer too
-- shape: filter
-- expect: agg
-- key: -
-- scenario: yes
SELECT o.status, count(*) AS n, count(o.customer_id) AS with_customer
FROM orders o
WHERE o.deleted_at IS NULL AND o.customer_id NOT IN (SELECT m.rank FROM map_vals m WHERE m.rank < 0)
GROUP BY o.status ORDER BY o.status NULLS FIRST
