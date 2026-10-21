-- the rank is read by the query too: its column stays in the output of the plan of the sub-select
-- shape: join
-- expect: any
-- key: -
-- scenario: yes
SELECT ranked.rn, count(*) AS n
FROM orders o JOIN (SELECT store_id, row_number() OVER (PARTITION BY kind ORDER BY n DESC, store_id) AS rn
           FROM (SELECT o2.store_id, s2.kind, count(*) AS n FROM orders o2 JOIN stores s2 ON s2.id = o2.store_id WHERE o2.deleted_at IS NULL GROUP BY 1, 2) c) ranked
  ON ranked.store_id = o.store_id
WHERE ranked.rn <= 3
GROUP BY 1
ORDER BY 1
