-- trap: a volatile condition in the ON clause keeps the normal plan
-- shape: trap
-- expect: plain
-- key: -
SELECT s.kind, count(*) AS rows
FROM stores s LEFT JOIN orders o ON o.store_id = s.id AND random() < 2
GROUP BY s.kind ORDER BY s.kind
