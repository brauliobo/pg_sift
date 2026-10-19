-- SELECT DISTINCT of two columns under a filter
-- shape: flat
-- expect: agg
-- key: -
-- scenario: yes
SELECT DISTINCT country_code, status FROM orders WHERE tenant_id < 40 ORDER BY country_code NULLS FIRST, status NULLS FIRST
