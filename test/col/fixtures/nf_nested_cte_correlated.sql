-- a WITH inside a lateral sub-select that reads the row around it changes with that row: not a lookup of the scan
-- shape: join
-- expect: any
-- key: -
-- scenario: yes
SELECT s.id, x.n FROM stores s CROSS JOIN LATERAL (
  WITH mine AS MATERIALIZED (SELECT id FROM orders WHERE store_id = s.id)
  SELECT count(*) AS n FROM orders o JOIN mine m ON m.id = o.id WHERE o.deleted_at IS NULL
) x
WHERE s.id <= 40
ORDER BY s.id
