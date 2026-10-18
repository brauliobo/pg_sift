-- a shared CTE whose arms the pushdown does not take (min of text, a window function) keeps the materialized plan
-- shape: trap
-- expect: plain
-- key: -
WITH base AS (
  SELECT o.id, o.lang, o.status, o.store_id FROM orders o WHERE o.deleted_at IS NULL)
SELECT 'lang' AS k, lang AS v, count(*) AS n, min(status) AS lo FROM base GROUP BY 2
UNION ALL SELECT 'store', store_id::text, count(*), max(status) FROM base GROUP BY 2
ORDER BY 1, 2
