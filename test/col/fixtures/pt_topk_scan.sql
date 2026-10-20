-- the ranked scan itself as the sub-select of the join, the sample of the coverage table (the score order ranks the top-k, the ties are the scan's)
-- shape: search
-- expect: agg
-- key: -
-- scenario: yes
SELECT status, count(*) AS n
FROM orders
WHERE id IN (SELECT id FROM orders WHERE memo @@> 'clean' ORDER BY sift.score(ctid) DESC LIMIT 100)
GROUP BY status ORDER BY status NULLS FIRST
