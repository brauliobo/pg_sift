-- quals: IN, NOT IN, BETWEEN and <> on integer, date and dictionary columns
-- shape: quals
-- expect: agg
-- key: -
SELECT lang, count(*) AS n
FROM orders
WHERE (tenant_id IN (1, 2, 3, 5, 8, 13, 21, 34, 55, 89, 144, 233, 377) OR source NOT IN ('web', 'pos', 'ios'))
  AND placed_at BETWEEN DATE '2023-03-01' AND DATE '2025-02-28' AND tenant_id BETWEEN 10 AND 300 AND placed_at <> DATE '2024-02-29'
GROUP BY lang
ORDER BY lang
