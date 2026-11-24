-- FULL JOIN with an empty fact: the dimension rows only
-- shape: join
-- expect: agg
-- guc: sift.agg_force = on
-- requires: SELECT current_setting('sift.agg_force', true) IS NOT NULL
-- key: -
SELECT d.label, count(*) AS rows, count(f.id) AS facts, count(d.w) AS weights
FROM pf_fact_empty f FULL JOIN pf_dim d ON f.a = d.a AND f.b = d.b
GROUP BY d.label ORDER BY d.label NULLS FIRST
