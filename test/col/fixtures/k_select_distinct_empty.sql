-- SELECT DISTINCT over no row
-- shape: flat
-- expect: agg
-- key: -
-- scenario: yes
SELECT DISTINCT status, tenant_id FROM orders WHERE id < 0 ORDER BY 1, 2
