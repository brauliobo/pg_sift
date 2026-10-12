-- Kernels module (src/sift_kernels_test.c): bit-exact SIMD checks through sift.simd.
CREATE FUNCTION sift._test_kern_digest(seed int8, iters int4) RETURNS int8
	AS 'MODULE_PATHNAME', 'sift_test_kern_digest' LANGUAGE C STRICT;
CREATE FUNCTION sift._test_kern_equiv(seed int8, iters int4) RETURNS int8
	AS 'MODULE_PATHNAME', 'sift_test_kern_equiv' LANGUAGE C STRICT;
CREATE FUNCTION sift._test_kern_bm25(tf int4, dl int4, k1 float4, b float4, avgdl float4,
									 w float4) RETURNS float4
	AS 'MODULE_PATHNAME', 'sift_test_kern_bm25' LANGUAGE C STRICT;
CREATE FUNCTION sift._test_kern_bound(tf int4, dl int4, k1 float4, b float4, avgdl float4) RETURNS int4
	AS 'MODULE_PATHNAME', 'sift_test_kern_bound' LANGUAGE C STRICT;
