-- a shared CTE whose two arms each hold an ordered json_agg of text: the generic aggregates with ORDER BY are pushed down (a tuplesort per group, as g_order_by), so both arms are SiftAgg over the table and
-- the CTE is not materialized: faster than the materialized plan with the same rows (4,000 rows: 3.9 ms against 7.1). The materialized plan of arms the pushdown cannot take is c_shared_cte_volatile.
-- shape: flat
-- expect: agg
-- key: -
WITH base AS (
  SELECT o.id, o.lang, o.status, o.tenant_id FROM orders o WHERE o.deleted_at IS NULL)
SELECT 'lang' AS k, lang AS v, count(*) AS n, json_agg(status ORDER BY id)::text AS statuses FROM base GROUP BY 2
UNION ALL SELECT 'store', tenant_id::text, count(*), json_agg(status ORDER BY id)::text FROM base GROUP BY 2
ORDER BY 1, 2
