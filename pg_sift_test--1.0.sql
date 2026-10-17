\echo Use "CREATE EXTENSION pg_sift_test" to load this file. \quit

-- Store module (src/sift_store_test.c). A sift_store_test index holds one test
-- segment per heap row: v bytes written through a BULK target at build time.
CREATE FUNCTION sift._test_am_handler(internal) RETURNS index_am_handler
	AS 'MODULE_PATHNAME', 'sift_test_am_handler' LANGUAGE C STRICT;
CREATE ACCESS METHOD sift_store_test TYPE INDEX HANDLER sift._test_am_handler;
CREATE OPERATOR CLASS sift._test_ops DEFAULT FOR TYPE int4 USING sift_store_test AS
	OPERATOR 1 = (int4, int4);

-- seed 0: zeros, no content generation (throughput runs; read with mode 2 or 3)
CREATE FUNCTION sift._test_seg_write(index regclass, nbytes int8, seed int8,
									 piece int4 DEFAULT 0, align int4 DEFAULT 0) RETURNS int8
	AS 'MODULE_PATHNAME', 'sift_test_seg_write' LANGUAGE C STRICT;
-- mode 0 replay + verify, 1 random verify, 2 page pointers, 3 1 MB copies
CREATE FUNCTION sift._test_seg_read(index regclass, gen int8, mode int4 DEFAULT 0,
									sleep_ms int4 DEFAULT 0) RETURNS int8
	AS 'MODULE_PATHNAME', 'sift_test_seg_read' LANGUAGE C STRICT;
CREATE FUNCTION sift._test_seg_drop(index regclass, gen int8) RETURNS int8
	AS 'MODULE_PATHNAME', 'sift_test_seg_drop' LANGUAGE C STRICT;
CREATE FUNCTION sift._test_segments(index regclass, OUT gen int8, OUT header int8,
									OUT size int8, OUT payload int8, OUT nextents int4)
	RETURNS SETOF record
	AS 'MODULE_PATHNAME', 'sift_test_segments' LANGUAGE C STRICT;
-- release: 0 none (orphans), 1 all, 2 every second extent
CREATE FUNCTION sift._test_alloc(index regclass, nblocks int4, count int4,
								 release int4 DEFAULT 1) RETURNS int8
	AS 'MODULE_PATHNAME', 'sift_test_alloc' LANGUAGE C STRICT;
CREATE FUNCTION sift._test_recycle(index regclass) RETURNS int4
	AS 'MODULE_PATHNAME', 'sift_test_recycle' LANGUAGE C STRICT;
CREATE FUNCTION sift._test_check(index regclass) RETURNS int8
	AS 'MODULE_PATHNAME', 'sift_test_check' LANGUAGE C STRICT;
CREATE FUNCTION sift._test_sweep(index regclass) RETURNS int8
	AS 'MODULE_PATHNAME', 'sift_test_sweep' LANGUAGE C STRICT;
CREATE FUNCTION sift._test_info(index regclass, OUT key text, OUT value int8)
	RETURNS SETOF record
	AS 'MODULE_PATHNAME', 'sift_test_info' LANGUAGE C STRICT;
-- pending: safexid set (not cleared by sift_recycle yet)
CREATE FUNCTION sift._test_freelist(index regclass, OUT page int8, OUT start int8, OUT nblocks int8,
									OUT pending bool) RETURNS SETOF record
	AS 'MODULE_PATHNAME', 'sift_test_freelist' LANGUAGE C STRICT;
CREATE FUNCTION sift._test_page(index regclass, blkno int8, OUT type int4, OUT gen int8,
								OUT lower int4, OUT upper int4)
	AS 'MODULE_PATHNAME', 'sift_test_page' LANGUAGE C STRICT;
CREATE FUNCTION sift._test_poke(index regclass, blkno int8, "offset" int4, value int8,
								width int4) RETURNS void
	AS 'MODULE_PATHNAME', 'sift_test_poke' LANGUAGE C STRICT;
CREATE FUNCTION sift._test_churn(index regclass, seed int8, maxbytes int8) RETURNS text
	AS 'MODULE_PATHNAME', 'sift_test_churn' LANGUAGE C STRICT;

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

-- Index build (src/sift_segment_sql.c): every posting decoded from the index.
CREATE FUNCTION sift._test_postings(index regclass, OUT gen int8, OUT term text, OUT tid tid,
									OUT tf int4, OUT dl int4) RETURNS SETOF record
	AS 'MODULE_PATHNAME', 'sift_test_postings' LANGUAGE C STRICT;

-- Filter masks (src/sift_filter_test.c): a TIDBitmap of tids and lossy blocks converted under a byte budget,
-- probed with sift_filter_has and, as a block mask, sift_filter_block (block_mismatches counts disagreements).
CREATE FUNCTION sift._test_filter(tids tid[], lossy_blocks int8[], maxbytes int8, probes tid[],
								  OUT superblocks int8, OUT exact_pages int8, OUT lossy_pages int8, OUT tuples int8,
								  OUT has bool[], OUT block_mismatches int8)
	AS 'MODULE_PATHNAME', 'sift_test_filter' LANGUAGE C STRICT;

-- Leak detection (src/sift_leak_test.c): leaves a resource behind on purpose so that scripts/leakcheck.sh is shown to fail.
-- kind: buffer, relcache, tupledesc, snapshot, lock, context, malloc
CREATE FUNCTION sift._test_leak(kind text, rel regclass DEFAULT 'pg_class') RETURNS void
	AS 'MODULE_PATHNAME', 'sift_test_leak' LANGUAGE C STRICT;

-- Read side (src/sift_read_test.c): what this backend's reads allocated and did; reset clears the counters.
CREATE FUNCTION sift._test_read_stats(reset bool DEFAULT false, OUT key text, OUT value int8) RETURNS SETOF record
	AS 'MODULE_PATHNAME', 'sift_test_read_stats' LANGUAGE C STRICT;
