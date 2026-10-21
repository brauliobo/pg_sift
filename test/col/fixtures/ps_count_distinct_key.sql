-- DISTINCT of the key of the scanned table: the dimension rows no fact matched hold no key, so they count 0
-- shape: join
-- expect: agg
-- key: -
SELECT d.label, count(*) AS rows, count(DISTINCT f.id) AS facts, count(f.id) AS plain
FROM pf_dim d LEFT JOIN pf_fact f ON f.a = d.a AND f.b = d.b AND f.v > 11
GROUP BY d.label ORDER BY d.label
