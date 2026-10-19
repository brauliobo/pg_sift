-- preagg: rollup: a map with a filter on its keys
-- shape: S2
-- expect: agg
-- key: -
-- scenario: yes
SELECT m.key, count(*) AS n, sum(m.value::float8) AS total
FROM orders o CROSS JOIN LATERAL jsonb_each_text(o.metrics) AS m
WHERE m.key IN ('mhead', 'packaging', 'value') OR m.key LIKE 'check%'
GROUP BY m.key
ORDER BY m.key
