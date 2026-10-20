-- Shredder kernels (src/col/shred_kern*.c): every SIMD level is bit-identical to the scalar specification. Levels above
-- what the CPU supports fall back to the best supported one, so the output does not depend on the host.
CREATE EXTENSION pg_sift;
CREATE EXTENSION pg_sift_test;

-- Digest of the kernel outputs over 400 random columns, per sift.simd level; the fixed value pins the specification itself.
SET sift.simd = scalar;
SELECT sift._test_shred_kern_level() AS level;
SELECT sift._test_shred_kern_digest(1, 400) AS scalar_digest \gset
SELECT :scalar_digest;
SET sift.simd = avx2;
SELECT sift._test_shred_kern_level() IN ('avx2', 'avx512') AS avx2_or_better, sift._test_shred_kern_digest(1, 400) = :scalar_digest AS avx2_same;
SET sift.simd = avx512;
SELECT sift._test_shred_kern_level() IN ('scalar', 'avx2', 'avx512') AS some_level, sift._test_shred_kern_digest(1, 400) = :scalar_digest AS avx512_same;
RESET sift.simd;
SELECT sift._test_shred_kern_digest(1, 400) = :scalar_digest AS auto_same;

-- Every level the CPU supports against scalar, case by case (lengths 0..1200, every state, extreme values, empty and long runs)
SELECT sift._test_shred_kern_equiv(42, 4000) AS mismatches;

-- The same documents shredded at every level: slots, parents (the column table's expand_parents), counts and min/max (the summaries) agree
CREATE TABLE d AS SELECT i AS id, jsonb_build_object('xs', (SELECT jsonb_agg(jsonb_build_object('a', j, 'b', j::float8 / 7, 'c', CASE WHEN j % 5 = 0 THEN NULL END,
							'l', (SELECT jsonb_agg(k) FROM generate_series(1, j % 4) k))) FROM generate_series(1, 40 + i) j)) AS doc FROM generate_series(1, 6) i;
\set spec 'xs[]{a int4, b float8, c int4, l[] int2}'
SET sift.simd = scalar;
CREATE TABLE r_scalar AS SELECT d.id, s.* FROM d, LATERAL sift._test_shred(d.doc, :'spec') s;
CREATE TABLE s_scalar AS SELECT d.id, s.* FROM d, LATERAL sift._test_shred_summary(d.doc, :'spec') s;
SET sift.simd = avx2;
CREATE TABLE r_avx2 AS SELECT d.id, s.* FROM d, LATERAL sift._test_shred(d.doc, :'spec') s;
CREATE TABLE s_avx2 AS SELECT d.id, s.* FROM d, LATERAL sift._test_shred_summary(d.doc, :'spec') s;
SET sift.simd = avx512;
CREATE TABLE r_avx512 AS SELECT d.id, s.* FROM d, LATERAL sift._test_shred(d.doc, :'spec') s;
CREATE TABLE s_avx512 AS SELECT d.id, s.* FROM d, LATERAL sift._test_shred_summary(d.doc, :'spec') s;
RESET sift.simd;
SELECT count(*) > 1000 AS rows,
	   (SELECT count(*) FROM (SELECT * FROM r_scalar EXCEPT SELECT * FROM r_avx2) x) AS slots_avx2,
	   (SELECT count(*) FROM (SELECT * FROM r_scalar EXCEPT SELECT * FROM r_avx512) x) AS slots_avx512,
	   (SELECT count(*) FROM (SELECT * FROM s_scalar EXCEPT SELECT * FROM s_avx2) x) AS summary_avx2,
	   (SELECT count(*) FROM (SELECT * FROM s_scalar EXCEPT SELECT * FROM s_avx512) x) AS summary_avx512
FROM r_scalar;

-- The throughput entry points run (the numbers are for bench/bench shred)
SELECT k, sift._test_shred_kern_rate(k, 4096, 20) > 0 AS runs
FROM unnest(ARRAY['state_counts', 'state_bits', 'minmax_i64', 'minmax_f64', 'expand_parents']) k;
SELECT sift._test_shred_kern_rate('nope', 16, 1);
-- the shared database of a plain installcheck goes on with the next test
SET client_min_messages = warning;
DROP SCHEMA public CASCADE;
CREATE SCHEMA public;
GRANT USAGE ON SCHEMA public TO PUBLIC;
RESET client_min_messages;
DROP EXTENSION pg_sift_test;
DROP EXTENSION pg_sift;
