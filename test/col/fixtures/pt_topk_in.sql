-- an aggregate over a ranked top-k: IN a sub-select that is a BM25 scan ordered by score (the ties broken by id so that both plans take the same rows)
-- shape: search
-- expect: agg
-- key: -
-- scenario: yes
SELECT status, count(*) AS n, sum(total) AS revenue
FROM orders
WHERE id IN (SELECT id FROM orders WHERE memo @@> 'clean' ORDER BY sift.score(ctid) DESC, id LIMIT 100)
GROUP BY status ORDER BY status NULLS FIRST
