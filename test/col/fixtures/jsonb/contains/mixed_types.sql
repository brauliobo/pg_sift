-- contains: a string and a JSON null of a text leaf whose rows hold numbers, booleans, objects and arrays as well (attributes.tier)
-- shape: flat
-- expect: agg
-- key: 1
SELECT o.status, count(*) AS n
FROM orders o
WHERE o.attributes @> '{"tier": "gold"}'
GROUP BY 1 ORDER BY 1 NULLS FIRST
