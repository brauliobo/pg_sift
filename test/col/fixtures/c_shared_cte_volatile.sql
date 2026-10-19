-- trap: a CTE with a volatile function is computed once and stays materialized (its arms agree with each other row for row)
-- shape: trap
-- expect: plain
-- key: -
WITH base AS (
  SELECT o.id, o.store_id, i.qty FROM orders o CROSS JOIN LATERAL jsonb_to_recordset(o.items) AS i(qty int)
  WHERE o.deleted_at IS NULL AND random() < 2)
SELECT a.store_id, a.units, b.units AS again FROM
  (SELECT store_id, sum(qty) AS units FROM base GROUP BY 1) a JOIN (SELECT store_id, sum(qty) AS units FROM base GROUP BY 1) b USING (store_id)
ORDER BY 1
