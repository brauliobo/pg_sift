-- a minimum score in the WHERE: the rows above it, ranked, with a LIMIT and an OFFSET
-- shape: search
-- expect: any
-- key: -
-- scenario: yes
SELECT id, round(sift.score(ctid)::numeric, 3) AS score FROM orders
WHERE memo @@> 'damaged OR broken OR refund' AND sift.score(ctid) > 2.5
ORDER BY sift.score(ctid) DESC, id LIMIT 40 OFFSET 5
