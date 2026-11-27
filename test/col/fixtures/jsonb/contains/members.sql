-- contains: attributes @> '{..}' as tests of the leaves of its members: a string of a text leaf and a boolean of a bool leaf, on the same document
-- shape: flat
-- expect: agg
-- key: 1
SELECT o.lang, count(*) AS n, sum(o.total) AS total
FROM orders o
WHERE o.attributes @> '{"channel": "web", "normalized": true}' AND o.deleted_at IS NULL
GROUP BY 1 ORDER BY 1
