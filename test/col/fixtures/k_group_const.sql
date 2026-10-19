-- a constant in the GROUP BY list adds no group
-- shape: flat
-- expect: agg
-- key: 1 2
-- scenario: yes
SELECT 'cur'::text AS label, country_code, count(*), sum(total) FROM orders GROUP BY 1, 2 ORDER BY 2 NULLS FIRST
