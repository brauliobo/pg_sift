-- an inner join to a VALUES list with a repeated key (an order counts once per row of the list) and a NULL key (it matches nothing)
-- shape: join
-- expect: agg
-- key: -
-- scenario: yes
SELECT m.label, count(*) AS n, sum(o.total) AS revenue, count(DISTINCT o.id) AS orders
FROM orders o JOIN (VALUES ('web', 'Online'), ('web', 'Web'), ('ios', 'Mobile'), ('android', 'Mobile'), (NULL, 'Unknown')) AS m(source, label) ON o.source = m.source
WHERE o.deleted_at IS NULL
GROUP BY m.label ORDER BY m.label
