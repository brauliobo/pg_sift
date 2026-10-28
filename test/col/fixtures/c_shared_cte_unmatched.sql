-- a shared CTE whose arms the pushdown does not take (an ordered json_agg of text) keeps the materialized plan
-- shape: trap
-- expect: plain
-- key: -
WITH base AS (
  SELECT o.id, o.lang, o.status, o.tenant_id FROM orders o WHERE o.deleted_at IS NULL)
SELECT 'lang' AS k, lang AS v, count(*) AS n, json_agg(status ORDER BY id)::text AS statuses FROM base GROUP BY 2
UNION ALL SELECT 'store', tenant_id::text, count(*), json_agg(status ORDER BY id)::text FROM base GROUP BY 2
ORDER BY 1, 2
