-- The radix sort of core/cmp.h gives the array of the comparison sort: one key and two, from one value to the whole range of int8, negative keys and the extremes of int8,
-- the sizes around each change of the digit (512 entries, 4,096 and 32,768), and keys that share a digit.
CREATE EXTENSION pg_sift;
CREATE EXTENSION pg_sift_test;

-- one key
SELECT n, bool_and(sift._test_radix_sort(n, b, -1, o, 0, s)) AS same
FROM unnest('{100,511,512,513,4095,4096,32767,32768,100000}'::int[]) n, unnest('{0,1,8,11,12,13,24,40,63,64}'::int[]) b,
     unnest('{0,-524288,-9223372036854775808}'::int8[]) o, generate_series(1, 2) s
GROUP BY n ORDER BY n;

-- two keys
SELECT n, bool_and(sift._test_radix_sort(n, b1, b2, o, o / 2, s)) AS same
FROM unnest('{600,5000,40000,70000}'::int[]) n, unnest('{0,3,12,24,64}'::int[]) b1, unnest('{0,5,12,24,40,64}'::int[]) b2,
     unnest('{0,-524288,-9223372036854775808}'::int8[]) o, generate_series(1, 2) s
GROUP BY n ORDER BY n;

DROP EXTENSION pg_sift_test;
DROP EXTENSION pg_sift;
