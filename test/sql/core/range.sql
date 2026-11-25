-- The bounds of core/range.h (the key of a partition) against an oracle that compares key tuples element by element: the order of the encoding, the depth at which two bounds differ,
-- the successor of a prefix, the overlap and containment of ranges, the span of the partitions of a tiling that hold a prefix and the heaviest set of ranges that are apart (against every subset), each over random cases of one to three components
-- (the extremes of int8, NULL, values between the bounds, heap addresses). Every failure count is 0, every check count is not.
CREATE EXTENSION pg_sift;
CREATE EXTENSION pg_sift_test;

SELECT s AS seed, r[1] AS order_checks, r[2] AS order_failed, r[3] AS next_checks, r[4] AS next_failed, r[5] AS span_checks, r[6] AS span_failed,
       r[7] AS range_checks, r[8] AS range_failed, r[9] AS depth_checks, r[10] AS depth_failed, r[11] AS apart_checks, r[12] AS apart_failed
FROM generate_series(1, 4) s, LATERAL (SELECT sift._test_range_check(s, 400) r) x;

-- The same with every key component ordered by the flags of key_order (1 = DESC, 2 = NULLS FIRST, 3 = both): the order and the successor of a prefix and the depth of the difference follow the flags (the checks of the rest read bounds of the default order)
SELECT f AS flags, r[1] AS order_checks, r[2] AS order_failed, r[3] AS next_checks, r[4] AS next_failed, r[5] AS span_checks, r[6] AS span_failed,
       r[7] AS range_checks, r[8] AS range_failed, r[9] AS depth_checks, r[10] AS depth_failed, r[11] AS apart_checks, r[12] AS apart_failed
FROM generate_series(1, 3) f, LATERAL (SELECT sift._test_range_check(f, 400, f) r) x;

DROP EXTENSION pg_sift_test;
DROP EXTENSION pg_sift;
