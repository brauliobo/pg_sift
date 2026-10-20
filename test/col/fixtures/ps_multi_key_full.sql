-- FULL JOIN on two keys: a fact with a NULL key or a pair the dimension lacks is a row of NULL dimension columns
-- shape: join
-- expect: agg
-- key: -
SELECT d.label, f.t, count(*) AS rows, count(f.id) AS facts, count(d.w) AS weights, sum(d.w) AS weight, sum(f.v) AS v
FROM pf_fact f FULL JOIN pf_dim d ON f.a = d.a AND f.b = d.b
GROUP BY d.label, f.t ORDER BY d.label NULLS FIRST, f.t NULLS FIRST
