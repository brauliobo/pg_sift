-- trap: the sub-select reads the order (LATERAL): it is a plan per row, not a list
-- shape: trap
-- expect: plain
-- key: -
SELECT k.kind, count(*) AS n
FROM orders o JOIN LATERAL (SELECT s.kind FROM tenants s WHERE s.id = o.tenant_id ORDER BY s.id LIMIT 1) k ON k.kind IS NOT NULL
GROUP BY k.kind ORDER BY k.kind
