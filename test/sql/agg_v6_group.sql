-- Group table beyond 16 byte keys (src/agg/ht_test.c): the growth threshold at a load of 7/8 (written out in the test, so a
-- changed constant fails it), wide keys and string views with state blocks of sums, minima, variance and a distinct set, and the
-- distinct set alone, of 64-bit values and of the bytes of wide ones (hashes forced to collide, merged, shipped as records). Each is the number of failed cases.
CREATE EXTENSION pg_sift;
CREATE EXTENSION pg_sift_test;

SELECT sift._test_ht_grow() AS grow_failures;
SELECT sift._test_ht_ext(3, 25) AS ext_failures;
SELECT sift._test_dist_set(9, 60) AS dist_failures;
SELECT sift._test_dist_set(1234, 60) AS dist_failures;

DROP EXTENSION pg_sift_test;
DROP EXTENSION pg_sift;
