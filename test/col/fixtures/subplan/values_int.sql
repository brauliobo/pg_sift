-- a VALUES list of integer keys (a repeated key and a NULL one) joined to an integer leaf, with a column of the list in an expression
-- shape: join
-- expect: agg
-- key: -
-- scenario: yes
SELECT m.band, o.status, count(*) AS n, sum(o.total * m.factor) AS weighted
FROM orders o JOIN (VALUES (1, 'a', 1.5::float8), (2, 'a', 2.0), (2, 'b', 0.5), (7, 'c', 1.0), (NULL, 'z', 9.0)) AS m(tenant_id, band, factor) ON o.tenant_id = m.tenant_id
WHERE o.deleted_at IS NULL
GROUP BY m.band, o.status ORDER BY m.band, o.status NULLS FIRST
