-- S4: time series of orders, units and revenue at four granularities (day, week, month, quarter), items expanded once
-- sets: K10 K400 K4000
-- key: 1 2
--@ pg
WITH base AS (
  SELECT o.id AS order_id, o.placed_at, i.qty, i.qty * (i.price - i.discount) AS net
  FROM orders o
  CROSS JOIN LATERAL jsonb_to_recordset(o.items) AS i(qty int, price float8, discount float8)
  WHERE o.store_id IN ({stores}) AND o.source IN ({sources}) AND o.deleted_at IS NULL AND {range(o.placed_at)})
SELECT 'day' AS grain, placed_at AS bucket, count(DISTINCT order_id) AS orders, sum(qty) AS units, sum(net) AS revenue FROM base GROUP BY 2
UNION ALL SELECT 'week', date_trunc('week', placed_at)::date, count(DISTINCT order_id), sum(qty), sum(net) FROM base GROUP BY 2
UNION ALL SELECT 'month', date_trunc('month', placed_at)::date, count(DISTINCT order_id), sum(qty), sum(net) FROM base GROUP BY 2
UNION ALL SELECT 'quarter', date_trunc('quarter', placed_at)::date, count(DISTINCT order_id), sum(qty), sum(net) FROM base GROUP BY 2
--@ flat
WITH base AS (
  SELECT i.order_id, i.placed_at, i.qty, i.qty * (i.price - i.discount) AS net
  FROM pg_items i
  WHERE i.store_id IN ({stores}) AND i.source IN ({sources}) AND i.deleted_at IS NULL AND {range(i.placed_at)})
SELECT 'day' AS grain, placed_at AS bucket, count(DISTINCT order_id) AS orders, sum(qty) AS units, sum(net) AS revenue FROM base GROUP BY 2
UNION ALL SELECT 'week', date_trunc('week', placed_at)::date, count(DISTINCT order_id), sum(qty), sum(net) FROM base GROUP BY 2
UNION ALL SELECT 'month', date_trunc('month', placed_at)::date, count(DISTINCT order_id), sum(qty), sum(net) FROM base GROUP BY 2
UNION ALL SELECT 'quarter', date_trunc('quarter', placed_at)::date, count(DISTINCT order_id), sum(qty), sum(net) FROM base GROUP BY 2
--@ ch
WITH base AS (
  SELECT i.order_id AS order_id, i.placed_at AS placed_at, i.qty AS qty, i.qty * (i.price - i.discount) AS net
  FROM items_tuned i
  WHERE i.store_id IN ({stores}) AND i.source IN ({sources}) AND i.deleted = 0 AND {range(i.placed_at)})
SELECT 'day' AS grain, placed_at AS bucket, count(DISTINCT order_id) AS orders, sum(qty) AS units, sum(net) AS revenue FROM base GROUP BY bucket
UNION ALL SELECT 'week', toMonday(placed_at) AS bucket, count(DISTINCT order_id), sum(qty), sum(net) FROM base GROUP BY bucket
UNION ALL SELECT 'month', toStartOfMonth(placed_at) AS bucket, count(DISTINCT order_id), sum(qty), sum(net) FROM base GROUP BY bucket
UNION ALL SELECT 'quarter', toStartOfQuarter(placed_at) AS bucket, count(DISTINCT order_id), sum(qty), sum(net) FROM base GROUP BY bucket
