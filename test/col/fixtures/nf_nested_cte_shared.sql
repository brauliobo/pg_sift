-- arms of a UNION ALL that read one CTE of the derived table that holds them (planned inline when the arms run on the index)
-- shape: S4
-- expect: any
-- key: 1 2
-- scenario: yes
SELECT * FROM (
  WITH base AS (
    SELECT o.id AS order_id, o.placed_at, i.qty
    FROM orders o CROSS JOIN LATERAL jsonb_to_recordset(o.items) AS i(qty int, price float8, discount float8)
    WHERE o.deleted_at IS NULL)
  SELECT 'day' AS grain, placed_at AS bucket, count(DISTINCT order_id) AS orders, sum(qty) AS units FROM base GROUP BY 2
  UNION ALL SELECT 'month', date_trunc('month', placed_at)::date, count(DISTINCT order_id), sum(qty) FROM base GROUP BY 2
) q
