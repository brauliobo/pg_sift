-- a condition on a column of the sub-select that PostgreSQL pushes into it (HAVING): nothing else reads the column and its plan outputs a NULL, the scan does not test the condition again
-- shape: join
-- expect: agg
-- key: -
SELECT o.status, count(*) AS n
FROM orders o JOIN (SELECT store_id, count(*) AS n FROM orders GROUP BY store_id) s ON o.store_id = s.store_id
WHERE s.n > 20
GROUP BY o.status ORDER BY o.status
