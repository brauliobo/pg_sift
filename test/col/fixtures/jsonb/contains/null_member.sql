-- contains: a JSON null member is the type null of its leaf, not a missing key nor a SQL NULL
-- shape: flat
-- expect: agg
-- key: 1
SELECT o.lang, count(*) AS n
FROM orders o
WHERE o.attributes @> '{"tier": null}'
GROUP BY 1 ORDER BY 1
