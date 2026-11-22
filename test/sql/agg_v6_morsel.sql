-- Guided work queue of the parallel scans (src/agg/par/morsel.c): the leader and 0, 1, 2, 4 and 8 workers take every granule of
-- parts whose sizes are as skewed as a Zipf law (half of the granules in one part, a quarter in the next, some parts empty)
-- from one cursor. Every granule is visited exactly once, every claim lies in one part and has the guided length (the part is
-- shared by all the participants, not given to one), and the claim that ends the list is one granule.
CREATE EXTENSION pg_sift;
CREATE EXTENSION pg_sift_test;
SET max_parallel_workers = 16;
SET max_parallel_workers_per_gather = 16;

SELECT w AS workers, p AS parts, r.launched = w AS launched_all, r.total, r.wrong AS granules_not_once, r.bad_claims,
       r.max_run, r.tail_run, r.claims > 0 AS claimed
FROM (VALUES (0, 7), (1, 7), (2, 7), (4, 7), (8, 7), (4, 1), (8, 16), (8, 3)) AS v(w, p),
     LATERAL sift._test_morsel_par(5, w, p, 6000) AS r
ORDER BY w, p;

-- more participants than granules, and a list that ends inside the first claim
SELECT w AS workers, g AS granules, r.launched = w AS launched_all, r.total, r.wrong AS granules_not_once, r.bad_claims, r.tail_run
FROM (VALUES (0, 1), (2, 1), (8, 5), (8, 33), (4, 64)) AS v(w, g),
     LATERAL sift._test_morsel_par(9, w, 2, g) AS r
ORDER BY w, g;

-- the claim shrinks as the list is consumed: one participant takes the peak run first and one granule last
SELECT r.max_run, r.tail_run, r.claims > r.total / 8 AS shrank
FROM sift._test_morsel_par(3, 0, 4, 20000) AS r;

DROP EXTENSION pg_sift_test;
DROP EXTENSION pg_sift;
