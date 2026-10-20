-- a maximum score in the WHERE (the best rows are the ones dropped), with another order and with the score order
-- shape: search
-- expect: any
-- key: -
-- scenario: yes
SELECT id, round(sift.score(ctid)::numeric, 3) AS score FROM orders
WHERE memo @@> 'damaged OR broken OR refund' AND sift.score(ctid) <= 2.5 AND status IS NOT NULL
ORDER BY sift.score(ctid) DESC, id LIMIT 25
