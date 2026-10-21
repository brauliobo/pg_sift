-- Column kernels (src/col/kern*.c): every SIMD level is bit-identical to the scalar specification on random inputs of every
-- slot (all widths, boundary thresholds, unaligned starts, n in 0..5000, garbage in the padding). A level the CPU lacks runs the
-- best one below it, so the output does not depend on the host.
CREATE EXTENSION pg_sift;
CREATE EXTENSION pg_sift_test;

SELECT sift._test_col_kernels(0, 1) AS scalar_1, sift._test_col_kernels(0, 2) AS scalar_2 \gset
SELECT :scalar_1 <> :scalar_2 AS seeds_differ;
SELECT l AS level, sift._test_col_kernels(l, 1) = :scalar_1 AND sift._test_col_kernels(l, 2) = :scalar_2 AS same_as_scalar
FROM generate_series(1, 2) l ORDER BY l;

-- The dispatch follows sift.simd
SET sift.simd = scalar;
SELECT sift._test_col_level() AS level, sift._test_col_kernels_active(1) = :scalar_1 AS same_as_scalar;
SET sift.simd = avx2;
SELECT sift._test_col_level() IN ('scalar', 'avx2') AS level_capped, sift._test_col_kernels_active(2) = :scalar_2 AS same_as_scalar;
SET sift.simd = avx512;
SELECT sift._test_col_level() IN ('scalar', 'avx2', 'avx512') AS level_known, sift._test_col_kernels_active(1) = :scalar_1 AS same_as_scalar;
RESET sift.simd;
SELECT sift._test_col_level() IN ('scalar', 'avx2', 'avx512') AS level_known, sift._test_col_kernels_active(2) = :scalar_2 AS same_as_scalar;

DROP EXTENSION pg_sift_test;
DROP EXTENSION pg_sift;
