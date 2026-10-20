-- trap: the sub-select returns more rows than the lookup tables may hold (sift.agg_lookup_max_rows): the normal plan stays
-- shape: trap
-- expect: plain
-- key: -
-- guc: sift.agg_lookup_max_rows = 100
SELECT s.kind, count(*) AS n
FROM orders o JOIN (SELECT id, kind FROM tenants WHERE id <= 300 LIMIT 300) s ON o.tenant_id = s.id
GROUP BY s.kind ORDER BY s.kind
