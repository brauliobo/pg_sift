-- a minimum score on the table of a join with a dimension table
-- shape: search
-- expect: any
-- key: -
-- scenario: yes
SELECT o.id, s.id AS store, round(sift.score(o.ctid)::numeric, 3) AS score FROM orders o JOIN tenants s ON s.id = o.tenant_id
WHERE o.memo @@> 'damaged OR broken OR refund' AND sift.score(o.ctid) > 2.5
ORDER BY sift.score(o.ctid) DESC, o.id LIMIT 30
