-- time-series-granularities: a time series at four granularities, every arm a GROUP BY of the same CTE (planned inline when the arms run on the index)
-- shape: time-series-granularities
-- expect: any
-- sets: K10 K400 K4000
-- key: 1 2
-- scenario: yes
WITH base AS (
  SELECT o.id AS order_id, o.placed_at, i.qty, i.qty * (i.price - i.discount) AS net
  FROM orders o
  CROSS JOIN LATERAL jsonb_to_recordset(o.items) AS i(qty int, price float8, discount float8)
  WHERE o.tenant_id IN ({tenants}) AND o.source IN ({sources}) AND o.deleted_at IS NULL AND {range(o.placed_at)})
SELECT 'day' AS grain, placed_at AS bucket, count(DISTINCT order_id) AS orders, sum(qty) AS units, sum(net) AS revenue FROM base GROUP BY 2
UNION ALL SELECT 'week', date_trunc('week', placed_at)::date, count(DISTINCT order_id), sum(qty), sum(net) FROM base GROUP BY 2
UNION ALL SELECT 'month', date_trunc('month', placed_at)::date, count(DISTINCT order_id), sum(qty), sum(net) FROM base GROUP BY 2
UNION ALL SELECT 'quarter', date_trunc('quarter', placed_at)::date, count(DISTINCT order_id), sum(qty), sum(net) FROM base GROUP BY 2
