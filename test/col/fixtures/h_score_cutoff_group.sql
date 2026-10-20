-- an aggregate over the rows above a minimum score: the pushdown must not take the match set and drop the cutoff
-- shape: search
-- expect: plain
-- key: -
-- scenario: yes
SELECT status, count(*) AS n, count(DISTINCT store_id) AS stores FROM orders
WHERE memo @@> 'damaged OR broken OR refund' AND sift.score(ctid) > 2.5
GROUP BY status ORDER BY status
