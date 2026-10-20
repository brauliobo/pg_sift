-- Scoring kernels (src/bm25/kernels*.c): every SIMD level is bit-identical to
-- the scalar specification. Levels above what the CPU supports fall back to
-- the best supported one, so the output does not depend on the host.
CREATE EXTENSION pg_sift;
CREATE EXTENSION pg_sift_test;

-- Digest of every kernel output over 300 random cases, per sift.simd level;
-- the fixed value pins the specification itself.
SET sift.simd = scalar;
SELECT sift._test_kern_digest(1, 300) AS scalar_digest \gset
SELECT :scalar_digest;
SET sift.simd = avx2;
SELECT sift._test_kern_digest(1, 300) = :scalar_digest AS avx2_same;
SET sift.simd = avx512;
SELECT sift._test_kern_digest(1, 300) = :scalar_digest AS avx512_same;
RESET sift.simd;
SELECT sift._test_kern_digest(1, 300) = :scalar_digest AS auto_same;

-- Every level the CPU supports against scalar, case by case
SELECT sift._test_kern_equiv(42, 3000) AS mismatches;

-- The kernels equal the float4 SQL evaluation of the documented formula:
-- K = k1 * (1 - b) + ((k1 * b) / avgdl) * dl, score = w * (tf / (tf + K))
CREATE TEMP TABLE params AS
	SELECT k1::float4, b::float4, avgdl::float4, w::float4
	FROM (VALUES (1.2, 0.75, 180.5, 3.5), (0, 0.75, 10, 1), (2, 1, 0.5, 7.25),
				 (1000, 0, 1, 0.001), (0.5, 0.25, 65535, 12), (1.2, 1e-6, 3.6e-15, 2)) v(k1, b, avgdl, w);
CREATE TEMP VIEW ref AS
	SELECT sift._test_kern_bm25(tf, dl, k1, b, avgdl, w) AS got,
		   w * (tf::float4 / (tf::float4 + ((k1 * (1::float4 - b)) + ((k1 * b) / avgdl) * dl::float4))) AS want
	FROM params, generate_series(1, 255, 6) tf, generate_series(0, 65535, 131) dl;
SET sift.simd = scalar;
SELECT count(*) AS pairs, count(*) FILTER (WHERE got <> want OR got = 'Infinity' OR got = 'NaN') AS mismatches FROM ref;
RESET sift.simd;
SELECT count(*) AS pairs, count(*) FILTER (WHERE got <> want OR got = 'Infinity' OR got = 'NaN') AS mismatches FROM ref;

-- u8 bounds: ceil(255 * tf / (tf + K)), clamped to [1, 255]
SELECT tf, dl, sift._test_kern_bound(tf, dl, 1.2, 0.75, 100) AS bound
FROM (VALUES (1, 0), (1, 100), (1, 65535), (2, 100), (255, 0), (255, 65535)) v(tf, dl);
SELECT sift._test_kern_bound(7, 50, 0, 0.75, 100) AS k1_zero;

DROP VIEW ref;
DROP TABLE params;
DROP EXTENSION pg_sift_test;
DROP EXTENSION pg_sift;
