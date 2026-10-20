-- trap: a sub-select whose column is random() (kept only by a condition that always holds)
-- shape: trap
-- expect: plain
-- key: -
SELECT s.kind, count(*) AS n
FROM orders o JOIN (SELECT id, kind FROM stores WHERE random() < 2 LIMIT 100) s ON o.store_id = s.id
GROUP BY s.kind ORDER BY s.kind
