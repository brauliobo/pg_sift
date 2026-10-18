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

-- Column kernels (src/sift_col_kern_test.c): a digest of all kernel outputs on random inputs, per SIMD level and through sift.simd.
CREATE FUNCTION sift._test_col_kernels(level int4, seed int4) RETURNS int8
	AS 'MODULE_PATHNAME', 'sift_test_col_kernels' LANGUAGE C STRICT;
CREATE FUNCTION sift._test_col_kernels_active(seed int4) RETURNS int8
	AS 'MODULE_PATHNAME', 'sift_test_col_kernels_active' LANGUAGE C STRICT;
CREATE FUNCTION sift._test_col_level() RETURNS text
	AS 'MODULE_PATHNAME', 'sift_test_col_level' LANGUAGE C STRICT;

-- Aggregation operators module (src/sift_aggkern_test.c): bit-exact SIMD checks through sift.simd.
CREATE FUNCTION sift._test_aggkern_digest(seed int8, cases int4) RETURNS int8
	AS 'MODULE_PATHNAME', 'sift_test_aggkern_digest' LANGUAGE C STRICT;
CREATE FUNCTION sift._test_aggkern_equiv(seed int8, cases int4) RETURNS int8
	AS 'MODULE_PATHNAME', 'sift_test_aggkern_equiv' LANGUAGE C STRICT;
CREATE FUNCTION sift._test_aggkern_level() RETURNS text
	AS 'MODULE_PATHNAME', 'sift_test_aggkern_level' LANGUAGE C STRICT;

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

-- Column engine L1 (src/sift_col_enc_test.c): random batches, chunks, zones and predicates against a naive evaluation of the same rows.
-- checks: how many comparisons ran; failures: how many differed (the first few are reported as warnings); encodings: bit mask of the
-- chunk encodings written (CONST, PACK of up to 7 / 9..15 / 17..31 bits, FOR 8, 16, 32, 64; bit 8: a text leaf of 16-bit codes; bit 9: a blob of the chunk codec), 767 when all were seen.
CREATE FUNCTION sift._test_col_enc_roundtrip(seed int4, nrows int4, rounds int4 DEFAULT 4, OUT checks int8, OUT failures int8, OUT encodings int8) RETURNS record
	AS 'MODULE_PATHNAME', 'sift_test_col_enc_roundtrip' LANGUAGE C STRICT;

-- Size of the encoded unit record of a full superblock (74,496 rows) for typical column shapes.
CREATE FUNCTION sift._test_col_enc_sizes(OUT shape text, OUT rows int8, OUT bytes int8, OUT bytes_per_row float8) RETURNS SETOF record
	AS 'MODULE_PATHNAME', 'sift_test_col_enc_sizes' LANGUAGE C STRICT;

-- Mutable lanes of the column engine (src/sift_col_mut_test.c), on an index of the BM25 access method created on an empty table.
-- Rows are generated from (seed, id) over a schema of orders with line items; prepare gives the index that schema's hash
-- (xor hash_xor: a stale definition), limit sets the mutable limit in kB.
CREATE FUNCTION sift._test_col_mut_prepare(index regclass, hash_xor int4 DEFAULT 0) RETURNS int8
	AS 'MODULE_PATHNAME', 'sift_test_col_mut_prepare' LANGUAGE C STRICT;
-- appends rows first .. first + n - 1; returns how many appends found the segment full. busy holds up to six lane tails while appending, so that
-- the rows go to other lanes: for one session at a time (two of them would wait for each other's tails)
CREATE FUNCTION sift._test_col_mut_append(index regclass, seed int8, first int4, n int4, busy bool DEFAULT false) RETURNS int8
	AS 'MODULE_PATHNAME', 'sift_test_col_mut_append' LANGUAGE C STRICT;
-- a big row (id % 1000 = 999) whose append fails after fail_after page links, leaving it torn
CREATE FUNCTION sift._test_col_mut_torn(index regclass, seed int8, id int4, fail_after int4) RETURNS void
	AS 'MODULE_PATHNAME', 'sift_test_col_mut_torn' LANGUAGE C STRICT;
CREATE FUNCTION sift._test_col_mut_toolong(index regclass) RETURNS void
	AS 'MODULE_PATHNAME', 'sift_test_col_mut_toolong' LANGUAGE C STRICT;
-- every record of the mutable segments through the reader, each checked against its generator
CREATE FUNCTION sift._test_col_mut_rows(index regclass, seed int8, OUT gen int8, OUT id int8, OUT deleted bool)
	RETURNS SETOF record
	AS 'MODULE_PATHNAME', 'sift_test_col_mut_rows' LANGUAGE C STRICT;
-- the live rows as batches (superblock, id), checked against the generator
CREATE FUNCTION sift._test_col_mut_batches(index regclass, seed int8, OUT gen int8, OUT sb int8, OUT id int8)
	RETURNS SETOF record
	AS 'MODULE_PATHNAME', 'sift_test_col_mut_batches' LANGUAGE C STRICT;
-- VACUUM's pass: flags the rows with id % modulus = 0; returns how many
CREATE FUNCTION sift._test_col_mut_delete(index regclass, modulus int4) RETURNS int8
	AS 'MODULE_PATHNAME', 'sift_test_col_mut_delete' LANGUAGE C STRICT;
CREATE FUNCTION sift._test_col_mut_seal(index regclass, force bool DEFAULT false) RETURNS int8
	AS 'MODULE_PATHNAME', 'sift_test_col_mut_seal' LANGUAGE C STRICT;
-- live: the rows sift_cmut_verify counts; extents: the blocks sift_cmut_pages reports (header included); lanes: lanes holding records
CREATE FUNCTION sift._test_col_mut_segments(index regclass, OUT gen int8, OUT kind text, OUT nrows int8, OUT npages int8,
											OUT live int8, OUT extents int8, OUT lanes int4)
	RETURNS SETOF record
	AS 'MODULE_PATHNAME', 'sift_test_col_mut_segments' LANGUAGE C STRICT;

-- Column source (src/sift_col_test.c). preds: "leaf:op:v1|v2;..." (eq ne lt le gt ge in notin null notnull prefix); "gN:leaf:op:v" is a
-- same-element predicate of group N ("gN!" none of the elements). flags: 1 skip the mutable segments, 2 skip the visibility check.
CREATE FUNCTION sift._test_col_tids(index regclass, preds text DEFAULT '', flags int4 DEFAULT 0, OUT tid tid, OUT sb int4) RETURNS SETOF record
	AS 'MODULE_PATHNAME', 'sift_test_col_tids' LANGUAGE C STRICT;
CREATE FUNCTION sift._test_col_read(index regclass, leaf int4, preds text DEFAULT '', flags int4 DEFAULT 0,
									OUT tid tid, OUT val text) RETURNS SETOF record
	AS 'MODULE_PATHNAME', 'sift_test_col_read' LANGUAGE C STRICT;
CREATE FUNCTION sift._test_col_zones(index regclass, leaf int4, preds text DEFAULT '',
									 OUT sb int4, OUT in_mutable bool, OUT nrows int4, OUT nvals int8, OUT nnull int8,
									 OUT min int8, OUT max int8, OUT sum_i int8, OUT exact bool) RETURNS SETOF record
	AS 'MODULE_PATHNAME', 'sift_test_col_zones' LANGUAGE C STRICT;
CREATE FUNCTION sift._test_col_counters(index regclass, preds text DEFAULT '', flags int4 DEFAULT 0,
										OUT rows int8, OUT segments int8, OUT mutable_rows int8, OUT units int8,
										OUT units_pruned int8, OUT rows_scanned int8, OUT rows_vetted int8,
										OUT heap_pages int8, OUT simd text) RETURNS record
	AS 'MODULE_PATHNAME', 'sift_test_col_counters' LANGUAGE C STRICT;

-- Shredder (src/sift_shred_test.c, docs/COL-SHRED.md)
CREATE FUNCTION sift._test_shred_spec(spec text) RETURNS text
	AS 'MODULE_PATHNAME', 'sift_test_shred_spec' LANGUAGE C STRICT;
-- kind: col (a slot), list (a list under a parent element: ord = first element, val = count), ovf (the overflow reasons of a column)
CREATE FUNCTION sift._test_shred(doc jsonb, spec text, max_elems int4 DEFAULT 0, max_bytes int4 DEFAULT 0,
								 OUT kind text, OUT path text, OUT parent int4, OUT ord int4, OUT key text,
								 OUT state text, OUT val text) RETURNS SETOF record
	AS 'MODULE_PATHNAME', 'sift_test_shred' LANGUAGE C STRICT;
CREATE FUNCTION sift._test_shred_count(doc jsonb, spec text) RETURNS int8
	AS 'MODULE_PATHNAME', 'sift_test_shred_count' LANGUAGE C STRICT;
CREATE FUNCTION sift._test_shred_summary(doc jsonb, spec text, OUT path text, OUT absent int4, OUT nulls int4,
										 OUT vals int4, OUT overflow int4, OUT min text, OUT max text) RETURNS SETOF record
	AS 'MODULE_PATHNAME', 'sift_test_shred_summary' LANGUAGE C STRICT;
CREATE FUNCTION sift._test_shred_kern_digest(seed int8, iters int4) RETURNS int8
	AS 'MODULE_PATHNAME', 'sift_test_shred_kern_digest' LANGUAGE C STRICT;
CREATE FUNCTION sift._test_shred_kern_equiv(seed int8, iters int4) RETURNS int8
	AS 'MODULE_PATHNAME', 'sift_test_shred_kern_equiv' LANGUAGE C STRICT;
CREATE FUNCTION sift._test_shred_kern_rate(kernel text, n int4, iters int4) RETURNS float8
	AS 'MODULE_PATHNAME', 'sift_test_shred_kern_rate' LANGUAGE C STRICT;

-- Column codec (src/sift_colcodec_test.c): chunks through the backend, sift.simd and the build's LZ4/zstd.
-- kind 0 integers, 1 float4 keys, 2 float8 keys; NULL elements are NULL slots (decode returns the filler there)
CREATE FUNCTION sift._test_cc_encode(keys int8[], kind int4, policy text) RETURNS bytea
	AS 'MODULE_PATHNAME', 'sift_test_cc_encode' LANGUAGE C STRICT;
CREATE FUNCTION sift._test_cc_decode(blob bytea, verify bool DEFAULT true) RETURNS int8[]
	AS 'MODULE_PATHNAME', 'sift_test_cc_decode' LANGUAGE C STRICT;
-- tag: 1 constant, 2 bit-packed, 3 plain bytes, 4 delta, 5 runs, 6 dictionary, 7 decimal floats, 8 wrapped, 9 common divisor
CREATE FUNCTION sift._test_cc_info(blob bytea, OUT kind int4, OUT n int4, OUT min int8, OUT max int8,
								   OUT tag int4, OUT bytes int4) RETURNS record
	AS 'MODULE_PATHNAME', 'sift_test_cc_info' LANGUAGE C STRICT;
CREATE FUNCTION sift._test_cc_filter(blob bytea, lo int8, hi int8, OUT rows int4[], OUT sum int8) RETURNS record
	AS 'MODULE_PATHNAME', 'sift_test_cc_filter' LANGUAGE C STRICT;
CREATE FUNCTION sift._test_cc_get(blob bytea, rownum int4) RETURNS int8
	AS 'MODULE_PATHNAME', 'sift_test_cc_get' LANGUAGE C STRICT;
-- whether the build has the library: 'lz4' or 'zstd' (SIFT_NO_LZ4=1 / SIFT_NO_ZSTD=1 leave one out)
CREATE FUNCTION sift._test_cc_has(alg text) RETURNS bool
	AS 'MODULE_PATHNAME', 'sift_test_cc_has' LANGUAGE C STRICT;
CREATE FUNCTION sift._test_cc_key(float8) RETURNS int8
	AS 'MODULE_PATHNAME', 'sift_test_cc_key' LANGUAGE C STRICT IMMUTABLE;
CREATE FUNCTION sift._test_cc_unkey(int8) RETURNS float8
	AS 'MODULE_PATHNAME', 'sift_test_cc_unkey' LANGUAGE C STRICT IMMUTABLE;
CREATE FUNCTION sift._test_cc_digest(seed int8, iters int4) RETURNS int8
	AS 'MODULE_PATHNAME', 'sift_test_cc_digest' LANGUAGE C STRICT;
CREATE FUNCTION sift._test_cc_equiv(seed int8, iters int4) RETURNS int8
	AS 'MODULE_PATHNAME', 'sift_test_cc_equiv' LANGUAGE C STRICT;
CREATE FUNCTION sift._test_cc_corrupt(seed int8, iters int4, OUT raised int8, OUT decoded int8) RETURNS record
	AS 'MODULE_PATHNAME', 'sift_test_cc_corrupt' LANGUAGE C STRICT;
