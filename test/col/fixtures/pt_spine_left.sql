-- trap: the spine is the preserved side of a LEFT JOIN and the orders are the nullable one: the dates without an order stay, the normal plan keeps them
-- shape: trap
-- expect: plain
-- key: -
SELECT d.day, count(o.id) AS n, sum(o.total) AS revenue
FROM (SELECT g::date AS day FROM generate_series(date '2025-03-01', date '2025-03-10', interval '1 day') g OFFSET 0) d
LEFT JOIN orders o ON o.placed_at = d.day AND o.deleted_at IS NULL
GROUP BY d.day ORDER BY d.day
