-- IN and NOT EXISTS against sub-selects with LIMIT: semi and anti joins of a plan
-- shape: join
-- expect: agg
-- key: -
-- scenario: yes
SELECT o.status, count(*) AS n, sum(o.total) AS revenue
FROM orders o
WHERE o.deleted_at IS NULL AND o.store_id IN (SELECT id FROM stores ORDER BY id LIMIT 40)
  AND NOT EXISTS (SELECT 1 FROM (SELECT id FROM stores ORDER BY id LIMIT 6) f WHERE f.id = o.store_id)
GROUP BY o.status ORDER BY o.status NULLS FIRST
