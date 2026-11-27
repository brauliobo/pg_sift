-- contains: NOT (attributes @> '{..}') is the complement over the documents, a document without the key included
-- shape: flat
-- expect: agg
-- key: 1
SELECT o.lang, count(*) AS n
FROM orders o
WHERE NOT (o.attributes @> '{"tier": "silver", "channel": "pos"}')
GROUP BY 1 ORDER BY 1
