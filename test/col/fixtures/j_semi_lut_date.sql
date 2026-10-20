-- a date leaf against the dates of a table, IN and NOT EXISTS (a table of dates with a NULL and duplicates)
-- shape: join
-- expect: agg
-- key: -
-- scenario: yes
SELECT o.status, count(*) AS n, min(o.placed_at) AS first_day
FROM orders o
WHERE o.deleted_at IS NULL AND o.placed_at IN (SELECT m.since FROM map_vals m WHERE m.rank IS NULL OR m.rank < 100)
GROUP BY o.status
UNION ALL
SELECT 'not', count(*), min(o.placed_at)
FROM orders o WHERE o.deleted_at IS NULL AND NOT EXISTS (SELECT 1 FROM map_vals m WHERE m.since = o.placed_at)
ORDER BY 1 NULLS FIRST
