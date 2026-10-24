-- DISTINCT of the key of an empty fact: every dimension row is unmatched, so every group counts 0
-- shape: join
-- expect: agg
-- guc: sift.agg_force = on
-- key: -
SELECT d.label, count(*) AS rows, count(DISTINCT f.id) AS facts
FROM pf_dim d LEFT JOIN pf_fact_empty f ON f.a = d.a AND f.b = d.b
GROUP BY d.label ORDER BY d.label
