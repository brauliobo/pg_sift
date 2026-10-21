-- EXISTS over the items in the nullable input of a calendar join: the days no order reaches stay with a count of 0
-- shape: join
-- expect: agg
-- key: -
-- scenario: yes
SELECT d.day, count(DISTINCT o.id) AS orders, sum(o.total) AS total
FROM dim_day d LEFT JOIN (
    SELECT o.id, o.placed_at, o.total FROM orders o JOIN tenants s ON s.id = o.tenant_id
    WHERE o.deleted_at IS NULL AND EXISTS (SELECT 1 FROM jsonb_to_recordset(o.items) AS i(category text, kind text, qty int, price float8, discount float8)
      WHERE i.kind = CASE s.kind WHEN 'online' THEN 'digital' ELSE 'standard' END)
  ) o ON o.placed_at = d.day
GROUP BY d.day ORDER BY d.day
