-- the empty grouping set has no key: the constant is not dropped (the output keeps its NULL there)
-- shape: flat
-- expect: any
-- key: -
-- scenario: yes
SELECT 'cur'::text AS label, status, count(*) FROM orders GROUP BY GROUPING SETS ((1, 2), ()) ORDER BY 1 NULLS FIRST, 2 NULLS FIRST
