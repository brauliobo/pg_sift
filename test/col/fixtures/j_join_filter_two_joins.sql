-- two inner joins, each on its own leaf, one with a second condition on a column of the table: each key set is a conjunct of the scan
-- shape: join
-- expect: agg
-- key: -
-- scenario: yes
SELECT s.country_code, o.status, count(*) AS n, count(DISTINCT o.store_id) AS stores
FROM orders o JOIN stores s ON s.id = o.store_id AND s.kind IN ('outlet', 'online')
JOIN stores s2 ON s2.id = o.store_id AND s2.name <> 'Store 7'
WHERE o.deleted_at IS NULL
GROUP BY s.country_code, o.status ORDER BY s.country_code, o.status NULLS FIRST
