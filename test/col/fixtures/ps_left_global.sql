-- no GROUP BY: the unmatched stores count as rows
-- shape: join
-- expect: agg
-- key: -
-- scenario: yes
SELECT count(*) AS rows, count(o.id) AS orders, count(DISTINCT s.id) AS stores, sum(o.total) AS revenue
FROM stores s LEFT JOIN orders o ON o.store_id = s.id AND o.total > 300
