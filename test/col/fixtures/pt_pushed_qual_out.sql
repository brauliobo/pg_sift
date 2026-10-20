-- a condition pushed into the sub-select on a column the query reads as well: the plan outputs it
-- shape: join
-- expect: agg
-- key: -
-- scenario: yes
SELECT o.status, max(s.n) AS busiest, count(*) AS n
FROM orders o JOIN (SELECT store_id, count(*) AS n FROM orders GROUP BY store_id) s ON o.store_id = s.store_id
WHERE s.n > 20
GROUP BY o.status ORDER BY o.status
