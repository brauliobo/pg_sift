-- an empty fact: every dimension row is unmatched, the aggregates over the fact are 0 or NULL
-- shape: join
-- expect: agg
-- key: -
SELECT d.label, count(*) AS rows, count(f.id) AS facts, sum(f.v) AS v, min(f.t) AS t
FROM pf_dim d LEFT JOIN pf_fact_empty f ON f.a = d.a AND f.b = d.b
GROUP BY d.label ORDER BY d.label
