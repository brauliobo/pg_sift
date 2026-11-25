-- count(*) of the rows above a minimum score: not a SiftCount of the match
-- shape: bm25
-- expect: plain
-- key: -
-- scenario: yes
SELECT count(*) AS n FROM orders WHERE memo @@> 'damaged OR broken OR refund' AND sift.score(ctid) > 2.5
