-- LEFT JOIN LATERAL whose own conditions (the sub-select's WHERE, the ON clause) say which items exist: an order none of whose items passes is one NULL tuple
-- shape: items-exists-same-element
-- expect: agg
-- key: -
-- scenario: yes
SELECT o.id, max(o.total) AS total, count(*) AS tuples, count(x.qty) AS items, avg(x.price) AS price
FROM orders o LEFT JOIN LATERAL (SELECT * FROM jsonb_to_recordset(o.items) AS i(category text, kind text, qty int, price float8, discount float8)
                                 WHERE i.qty IS NOT NULL AND i.qty > 2) x ON x.price IS NOT NULL
WHERE o.tenant_id < 150
GROUP BY o.id ORDER BY o.id
