-- a semi join whose own condition reads another leaf of the row cannot be one table of keys: the generic join answers it, 
-- shape: join
-- expect: agg
-- key: -
-- scenario: yes
SELECT o.status, count(*) AS n
FROM orders o
WHERE o.deleted_at IS NULL AND EXISTS (SELECT 1 FROM stores s WHERE s.id = o.store_id AND s.country_code = o.country_code)
GROUP BY o.status ORDER BY o.status NULLS FIRST
