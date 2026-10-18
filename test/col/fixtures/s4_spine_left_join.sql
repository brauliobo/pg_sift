-- S4: a date spine (generate_series) left joined to the monthly counts, empty months included
-- shape: S4
-- expect: any
-- sets: K10
-- key: -
SELECT s.month::date AS month, coalesce(x.orders, 0) AS orders, x.avg_total
FROM generate_series(DATE '2024-01-01', DATE '2025-10-01', interval '1 month') s(month)
LEFT JOIN (SELECT date_trunc('month', o.placed_at)::date AS month, count(*) AS orders, avg(o.tenant_id) AS avg_total
           FROM orders o WHERE o.tenant_id IN ({tenants}) AND o.deleted_at IS NULL GROUP BY 1) x ON x.month = s.month::date
ORDER BY 1
