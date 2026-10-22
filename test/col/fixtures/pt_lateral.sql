-- a LATERAL sub-select that reads the order, one row by the unique key (LIMIT 1): a first-row lookup, a list of the stores the orders reach
-- shape: join
-- expect: agg
-- key: -
SELECT k.kind, count(*) AS n
FROM orders o JOIN LATERAL (SELECT s.kind FROM stores s WHERE s.id = o.store_id ORDER BY s.id LIMIT 1) k ON k.kind IS NOT NULL
GROUP BY k.kind ORDER BY k.kind
