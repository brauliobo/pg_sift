-- constants between the keys, an integer one, and an expression key
-- shape: flat
-- expect: agg
-- key: 1 3 4
-- scenario: yes
SELECT status, 7 AS seven, source, 'x'::text AS tag, count(*), avg(total) FROM orders GROUP BY status, 2, source, 4 ORDER BY 1 NULLS FIRST, 3 NULLS FIRST
