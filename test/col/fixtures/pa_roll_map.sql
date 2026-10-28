-- preagg: rollup: the keys of a map (metrics) as the group with count, sum, avg, min and max of the value
-- shape: item-summary-rollup
-- expect: agg
-- key: -
-- scenario: yes
SELECT m.key, count(*) AS n, sum(m.value::float8) AS total, avg(m.value::float8) AS mean, min(m.value::float8) AS least, max(m.value::float8) AS most
FROM orders o CROSS JOIN LATERAL jsonb_each_text(o.metrics) AS m
GROUP BY m.key
ORDER BY m.key
