-- the filtered scan returns the rows of the subquery that filters on the score: equal counts, nothing missing either way
-- shape: search
-- expect: plain
-- key: -
-- scenario: yes
WITH f AS (SELECT id FROM orders WHERE memo @@> 'damaged OR broken OR refund' AND sift.score(ctid) > 2.5),
	 r AS (SELECT id FROM (SELECT id, sift.score(ctid) AS s FROM orders WHERE memo @@> 'damaged OR broken OR refund' OFFSET 0) q WHERE s > 2.5)
SELECT (SELECT count(*) FROM f) AS n_cut, (SELECT count(*) FROM r) AS n_ref,
	   (SELECT count(*) FROM (SELECT id FROM f EXCEPT ALL SELECT id FROM r) d) AS only_cut,
	   (SELECT count(*) FROM (SELECT id FROM r EXCEPT ALL SELECT id FROM f) d) AS only_ref
