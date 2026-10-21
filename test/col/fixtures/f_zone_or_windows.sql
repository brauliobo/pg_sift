-- a period and the same one a year earlier as an OR of ranges of the date leaf, and an IN list of dates: the zones are tested for each disjunct
-- shape: quals
-- expect: agg
-- key: -
-- scenario: yes
SELECT source, count(*) AS n, sum(tenant_id) AS s, count(DISTINCT customer_id) AS c
FROM orders
WHERE deleted_at IS NULL AND ((placed_at >= DATE '2025-03-01' AND placed_at <= DATE '2025-03-31') OR (placed_at >= DATE '2024-03-01' AND placed_at <= DATE '2024-03-31')
      OR placed_at IN (DATE '2022-06-15', DATE '2023-06-15', DATE '2025-09-09'))
GROUP BY source ORDER BY source
