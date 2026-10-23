-- the empty grouping set has no key: the constant is not dropped (the output keeps its NULL there); before PostgreSQL 18 an ORDER BY 1 on the constant is no order at all
-- (the planner drops it), so the rows are ordered by the other columns
-- shape: flat
-- expect: any
-- key: -
-- scenario: yes
SELECT 'cur'::text AS label, status, count(*) FROM orders GROUP BY GROUPING SETS ((1, 2), ()) ORDER BY 2 NULLS FIRST, 3
