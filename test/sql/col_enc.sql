-- Column engine L1 (src/sift_col_enc.c): schema, dictionaries, batches, chunks, zones, predicates and unit records, against a naive
-- evaluation of the same random rows. Prints only counts and ok flags, so the output is the same on every machine.
CREATE EXTENSION pg_sift;
CREATE EXTENSION pg_sift_test;

-- Small and large batches, many seeds: every storage class, NULLs, overflow values, arrays, dense and sparse pages, strings, every
-- chunk encoding (encodings = 1791: every width class of CONST, PACK and FOR written, and the chunk codec's blobs), every predicate operator, zones, merges, corrupt records
SELECT sum(checks) > 10000 AS ran, sum(failures) AS failures, bit_or(encodings) = 1791 AS all_encodings
FROM generate_series(1, 8) s, LATERAL sift._test_col_enc_roundtrip(s, 300, 10);
SELECT checks > 10000 AS ran, failures FROM sift._test_col_enc_roundtrip(2, 5000, 10);
SELECT checks > 1000 AS ran, failures, encodings & 256 = 256 AS wide_dictionary FROM sift._test_col_enc_roundtrip(3, 74496, 2);
SELECT checks > 1000 AS ran, failures, encodings & 256 = 256 AS wide_dictionary FROM sift._test_col_enc_roundtrip(12, 74496, 1);
SELECT checks > 1000 AS ran, failures FROM sift._test_col_enc_roundtrip(4, 1, 60);
SELECT sum(failures) AS failures, count(*) AS runs
FROM generate_series(10, 40) s, LATERAL sift._test_col_enc_roundtrip(s, 1 + (s * 997) % 4000, 3);

-- The same rows through every SIMD level of the column kernels: the answers do not depend on it
SET sift.simd = scalar;
SELECT checks AS scalar_checks, failures FROM sift._test_col_enc_roundtrip(5, 2000, 8) \gset
SELECT failures FROM sift._test_col_enc_roundtrip(5, 2000, 8);
SET sift.simd = avx2;
SELECT checks = :scalar_checks AS avx2_same, failures FROM sift._test_col_enc_roundtrip(5, 2000, 8);
SET sift.simd = avx512;
SELECT checks = :scalar_checks AS avx512_same, failures FROM sift._test_col_enc_roundtrip(5, 2000, 8);
RESET sift.simd;


-- Sizes of a full superblock (74,496 rows): a dictionary of 20 values takes 5 bits per row, a month-clustered date 5, a boolean 1, a
-- constant nothing, a float or a random timestamp 8 bytes; the row directory of 256 dense pages is 1.6 kB
SELECT shape, bytes_per_row < limit_ AS small_enough
FROM sift._test_col_enc_sizes() s JOIN (VALUES
	('status, dictionary of 20 values', 0.7), ('order date, clustered in one month', 0.7), ('quantity, 1..5', 0.45), ('boolean', 0.2),
	('constant', 0.03), ('sequential id', 2.2), ('price, float8 with 2 decimals', 8.1)) v(name, limit_) ON s.shape = v.name
ORDER BY shape COLLATE "C";

DROP EXTENSION pg_sift_test;
DROP EXTENSION pg_sift;
