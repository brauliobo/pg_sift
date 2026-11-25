-- a NULL key of the dimension matches no fact (and a NULL key of a fact no dimension row), a single key
-- shape: join
-- expect: agg
-- key: -
SELECT d.a, count(*) AS rows, count(f.id) AS facts
FROM pf_dim d LEFT JOIN pf_fact f ON f.a = d.a
GROUP BY d.a ORDER BY d.a NULLS FIRST
