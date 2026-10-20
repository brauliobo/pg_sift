-- a VALUES list is the preserved side: its rows with no order are groups too (and a duplicate and a NULL key)
-- shape: join
-- expect: agg
-- key: -
-- scenario: yes
SELECT v.tag, count(*) AS rows, count(o.id) AS orders, sum(o.total) AS revenue
FROM (VALUES (1, 'a'), (2, 'b'), (2, 'b2'), (5, 'c'), (NULL, 'n'), (100000, 'none')) v(id, tag) LEFT JOIN orders o ON o.tenant_id = v.id
GROUP BY v.tag ORDER BY v.tag
