-- S4: orders per quarter and, for the last two months, per day (the granularities of one report as a UNION ALL)
-- shape: S4
-- expect: any
-- sets: K4000
-- key: -
SELECT 'quarter' AS grain, date_trunc('quarter', o.placed_at)::date AS bucket, count(*) AS orders FROM orders o
WHERE o.store_id IN ({stores}) AND o.deleted_at IS NULL GROUP BY 2
UNION ALL
SELECT 'day', o.placed_at, count(*) FROM orders o
WHERE o.store_id IN ({stores}) AND o.deleted_at IS NULL AND o.placed_at >= DATE '2025-09-01' GROUP BY 2
ORDER BY 1, 2
