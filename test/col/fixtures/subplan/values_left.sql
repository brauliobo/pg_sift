-- a LEFT JOIN to a VALUES list with a repeated and a NULL key: an order without a match is kept once with a NULL label
-- shape: join
-- expect: agg
-- key: -
-- scenario: yes
SELECT m.label, count(*) AS n, sum(o.total) AS revenue, count(m.label) AS labelled
FROM orders o LEFT JOIN (VALUES ('web', 'Online'), ('web', 'Web'), ('ios', 'Mobile'), ('android', 'Mobile'), (NULL, 'Unknown')) AS m(source, label) ON o.source = m.source
WHERE o.deleted_at IS NULL
GROUP BY m.label ORDER BY m.label NULLS FIRST
