-- IN and NOT EXISTS on an integer leaf against the keys of a table (a byte table over the keys): the table's own conditions on its rows, once per row
-- shape: join
-- expect: agg
-- key: -
-- scenario: yes
SELECT o.status, count(*) AS n, sum(o.total) AS revenue, count(DISTINCT o.store_id) AS stores
FROM orders o
WHERE o.deleted_at IS NULL AND o.store_id IN (SELECT s.id FROM stores s WHERE s.kind IN ('outlet', 'popup') AND s.name <> 'Store 7')
  AND NOT EXISTS (SELECT 1 FROM stores s2 WHERE s2.id = o.store_id AND s2.country_code = 'US')
GROUP BY o.status ORDER BY o.status NULLS FIRST
