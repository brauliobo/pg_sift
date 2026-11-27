-- contains: attributes -> 'k' = 'scalar' (and its subscript) is the test of one member
-- shape: flat
-- expect: agg
-- key: 1
SELECT o.lang, count(*) AS n
FROM orders o
WHERE o.attributes['channel'] = '"mobile"' AND o.attributes -> 'tier' = 'null'
GROUP BY 1 ORDER BY 1
