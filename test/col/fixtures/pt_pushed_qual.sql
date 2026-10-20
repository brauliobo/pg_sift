-- trap: a condition on a column of the sub-select that PostgreSQL pushes into it (HAVING), so nothing else reads the column and its plan outputs a NULL
-- shape: trap
-- expect: plain
-- key: -
SELECT o.status, count(*) AS n
FROM orders o JOIN (SELECT tenant_id, count(*) AS n FROM orders GROUP BY tenant_id) s ON o.tenant_id = s.tenant_id
WHERE s.n > 20
GROUP BY o.status ORDER BY o.status
