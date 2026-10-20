-- a key of two columns: the dimension rows with a NULL part or a pair no fact row has are unmatched, the pair with two dimension rows repeats its facts
-- shape: join
-- expect: agg
-- key: -
SELECT d.label, count(*) AS rows, count(f.id) AS facts, sum(f.v) AS v, avg(f.f) AS f, count(DISTINCT f.t) AS texts
FROM pf_dim d LEFT JOIN pf_fact f ON f.a = d.a AND f.b = d.b
GROUP BY d.label ORDER BY d.label
