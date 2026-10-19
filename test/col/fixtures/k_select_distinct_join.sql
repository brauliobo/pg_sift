-- SELECT DISTINCT of a column of a dimension table joined to the orders
-- shape: flat
-- expect: agg
-- key: -
-- scenario: yes
SELECT DISTINCT s.kind, o.status FROM orders o JOIN stores s ON s.id = o.store_id ORDER BY 1 NULLS FIRST, 2 NULLS FIRST
