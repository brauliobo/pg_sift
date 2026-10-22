\echo Use "CREATE EXTENSION pg_sift_test" to load this file. \quit

-- The limit registry (src/core/limits.h): each limit with its default and the value sift.test_limits gives it in this session.
CREATE FUNCTION sift._test_limits(OUT id text, OUT macro text, OUT dflt int8, OUT effective int8, OUT hooked bool)
	RETURNS SETOF record AS 'MODULE_PATHNAME', 'sift_test_limits' LANGUAGE C STRICT;

-- Store module (src/core/store_test.c). A sift_store_test index holds one test
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
-- blocks given back by VACUUM's truncation of the free blocks at the end of the file (NOTICE says what it did)
CREATE FUNCTION sift._test_truncate(index regclass) RETURNS int8
	AS 'MODULE_PATHNAME', 'sift_test_truncate' LANGUAGE C STRICT;
CREATE FUNCTION sift._test_info(index regclass, OUT key text, OUT value int8)
	RETURNS SETOF record
	AS 'MODULE_PATHNAME', 'sift_test_info' LANGUAGE C STRICT;
CREATE FUNCTION sift._test_maint_failures(index regclass) RETURNS int4
	AS 'MODULE_PATHNAME', 'sift_test_maint_failures' LANGUAGE C STRICT;
-- the metapage as an older build wrote it ('old') or with a failure note of garbage ('junk')
CREATE FUNCTION sift._test_meta_note(index regclass, mode text) RETURNS void
	AS 'MODULE_PATHNAME', 'sift_test_meta_note' LANGUAGE C STRICT;
-- longest wait of this backend for the maintenance of others since the last call, in microseconds
CREATE FUNCTION sift._test_maint_wait_us() RETURNS int8
	AS 'MODULE_PATHNAME', 'sift_test_maint_wait_us' LANGUAGE C STRICT;
-- directory updates of this process since sift.test_crash_dir was set (the named crash points of the matrix in test/t/crash_matrix.sh)
CREATE FUNCTION sift._test_dir_updates() RETURNS int4
	AS 'MODULE_PATHNAME', 'sift_test_dir_updates_count' LANGUAGE C STRICT;
-- copied pages of VACUUM's bounded relocation (with the current sift.vacuum_rewrite_limit)
CREATE FUNCTION sift._test_space(index regclass) RETURNS int8
	AS 'MODULE_PATHNAME', 'sift_test_space' LANGUAGE C STRICT;
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

-- Kernels module (src/bm25/kernels_test.c): bit-exact SIMD checks through sift.simd.
CREATE FUNCTION sift._test_kern_digest(seed int8, iters int4) RETURNS int8
	AS 'MODULE_PATHNAME', 'sift_test_kern_digest' LANGUAGE C STRICT;
CREATE FUNCTION sift._test_kern_equiv(seed int8, iters int4) RETURNS int8
	AS 'MODULE_PATHNAME', 'sift_test_kern_equiv' LANGUAGE C STRICT;
CREATE FUNCTION sift._test_kern_bm25(tf int4, dl int4, k1 float4, b float4, avgdl float4,
									 w float4) RETURNS float4
	AS 'MODULE_PATHNAME', 'sift_test_kern_bm25' LANGUAGE C STRICT;
CREATE FUNCTION sift._test_kern_bound(tf int4, dl int4, k1 float4, b float4, avgdl float4) RETURNS int4
	AS 'MODULE_PATHNAME', 'sift_test_kern_bound' LANGUAGE C STRICT;

-- Column kernels (src/col/kern_test.c): a digest of all kernel outputs on random inputs, per SIMD level and through sift.simd.
CREATE FUNCTION sift._test_col_kernels(level int4, seed int4) RETURNS int8
	AS 'MODULE_PATHNAME', 'sift_test_col_kernels' LANGUAGE C STRICT;
CREATE FUNCTION sift._test_col_kernels_active(seed int4) RETURNS int8
	AS 'MODULE_PATHNAME', 'sift_test_col_kernels_active' LANGUAGE C STRICT;
CREATE FUNCTION sift._test_col_level() RETURNS text
	AS 'MODULE_PATHNAME', 'sift_test_col_level' LANGUAGE C STRICT;

-- Aggregation operators module (src/agg/kern_test.c): bit-exact SIMD checks through sift.simd.
CREATE FUNCTION sift._test_aggkern_digest(seed int8, cases int4) RETURNS int8
	AS 'MODULE_PATHNAME', 'sift_test_aggkern_digest' LANGUAGE C STRICT;
CREATE FUNCTION sift._test_aggkern_equiv(seed int8, cases int4) RETURNS int8
	AS 'MODULE_PATHNAME', 'sift_test_aggkern_equiv' LANGUAGE C STRICT;
CREATE FUNCTION sift._test_aggkern_level() RETURNS text
	AS 'MODULE_PATHNAME', 'sift_test_aggkern_level' LANGUAGE C STRICT;

-- Index build (src/bm25/segment_sql.c): every posting decoded from the index.
CREATE FUNCTION sift._test_postings(index regclass, OUT gen int8, OUT term text, OUT tid tid,
									OUT tf int4, OUT dl int4) RETURNS SETOF record
	AS 'MODULE_PATHNAME', 'sift_test_postings' LANGUAGE C STRICT;

-- Where the bytes of the immutable segments go, by part
CREATE FUNCTION sift._test_segment_parts(index regclass, OUT gen int8, OUT part text, OUT bytes int8, OUT items int8)
	RETURNS SETOF record
	AS 'MODULE_PATHNAME', 'sift_test_segment_parts' LANGUAGE C STRICT;

CREATE FUNCTION sift._test_segment_formats(index regclass, OUT gen int8, OUT kind text, OUT format int4)
	RETURNS SETOF record
	AS 'MODULE_PATHNAME', 'sift_test_segment_formats' LANGUAGE C STRICT;

-- Term blobs (src/bm25/segment_sql.c): a term's blob and dictionary lengths as the query reads them, and that read over bytes the test
-- damaged (the query path does not run the exhaustive checks of sift.verify, so it follows the blob's own lengths)
CREATE FUNCTION sift._test_term_blob(index regclass, term text, OUT blob bytea, OUT flags int4, OUT df int4,
									 OUT codec_len int4, OUT nnib int4)
	AS 'MODULE_PATHNAME', 'sift_test_term_blob' LANGUAGE C STRICT;
CREATE FUNCTION sift._test_blob_check(blob bytea) RETURNS text
	AS 'MODULE_PATHNAME', 'sift_test_blob_check' LANGUAGE C STRICT;
CREATE FUNCTION sift._test_blob_read(blob bytea, flags int4, df int4, codec_len int4, nnib int4,
									 OUT postings int8, OUT tf_sum int8)
	AS 'MODULE_PATHNAME', 'sift_test_blob_read' LANGUAGE C STRICT;

-- Filter masks (src/core/filter_test.c): a TIDBitmap of tids and lossy blocks converted under a byte budget,
-- probed with sift_filter_has and, as a block mask, sift_filter_block (block_mismatches counts disagreements).
CREATE FUNCTION sift._test_filter(tids tid[], lossy_blocks int8[], maxbytes int8, probes tid[],
								  OUT superblocks int8, OUT exact_pages int8, OUT lossy_pages int8, OUT tuples int8,
								  OUT has bool[], OUT block_mismatches int8)
	AS 'MODULE_PATHNAME', 'sift_test_filter' LANGUAGE C STRICT;

-- Leak detection (src/core/leak_test.c): leaves a resource behind on purpose so that scripts/leakcheck.sh is shown to fail.
-- kind: buffer, relcache, tupledesc, snapshot, lock, context, malloc
CREATE FUNCTION sift._test_leak(kind text, rel regclass DEFAULT 'pg_class') RETURNS void
	AS 'MODULE_PATHNAME', 'sift_test_leak' LANGUAGE C STRICT;

-- Read side (src/bm25/read_test.c): what this backend's reads allocated and did; reset clears the counters.
CREATE FUNCTION sift._test_read_stats(reset bool DEFAULT false, OUT key text, OUT value int8) RETURNS SETOF record
	AS 'MODULE_PATHNAME', 'sift_test_read_stats' LANGUAGE C STRICT;

-- Column engine L1 (src/col/enc_test.c): random batches, chunks, zones and predicates against a naive evaluation of the same rows.
-- checks: how many comparisons ran; failures: how many differed (the first few are reported as warnings); encodings: bit mask of the
-- chunk encodings written (CONST, PACK of up to 7 / 9..15 / 17..31 bits, FOR 8, 16, 32, 64; bit 8: a text leaf of 16-bit codes; bit 9: a blob of the chunk codec), 767 when all were seen.
CREATE FUNCTION sift._test_col_enc_roundtrip(seed int4, nrows int4, rounds int4 DEFAULT 4, OUT checks int8, OUT failures int8, OUT encodings int8) RETURNS record
	AS 'MODULE_PATHNAME', 'sift_test_col_enc_roundtrip' LANGUAGE C STRICT;

-- Size of the encoded unit record of a full superblock (74,496 rows) for typical column shapes.
CREATE FUNCTION sift._test_col_enc_sizes(OUT shape text, OUT rows int8, OUT bytes int8, OUT bytes_per_row float8) RETURNS SETOF record
	AS 'MODULE_PATHNAME', 'sift_test_col_enc_sizes' LANGUAGE C STRICT;

-- Write buffer lanes of the column engine (src/col/wbuf_test.c), on an index of the BM25 access method created on an empty table.
-- Rows are generated from (seed, id) over a schema of orders with line items; prepare gives the index that schema's hash
-- (xor hash_xor: a stale definition), limit sets the write buffer limit in kB.
CREATE FUNCTION sift._test_col_wbuf_prepare(index regclass, hash_xor int4 DEFAULT 0) RETURNS int8
	AS 'MODULE_PATHNAME', 'sift_test_col_wbuf_prepare' LANGUAGE C STRICT;
-- appends rows first .. first + n - 1; returns how many appends found the segment full. busy holds up to six lane tails while appending, so that
-- the rows go to other lanes: for one session at a time (two of them would wait for each other's tails)
CREATE FUNCTION sift._test_col_wbuf_append(index regclass, seed int8, first int4, n int4, busy bool DEFAULT false) RETURNS int8
	AS 'MODULE_PATHNAME', 'sift_test_col_wbuf_append' LANGUAGE C STRICT;
-- a big row (id % 1000 = 999) whose append fails after fail_after page links, leaving it torn
CREATE FUNCTION sift._test_col_wbuf_torn(index regclass, seed int8, id int4, fail_after int4) RETURNS void
	AS 'MODULE_PATHNAME', 'sift_test_col_wbuf_torn' LANGUAGE C STRICT;
CREATE FUNCTION sift._test_col_wbuf_long(index regclass) RETURNS void
	AS 'MODULE_PATHNAME', 'sift_test_col_wbuf_long' LANGUAGE C STRICT;
-- every record of the write buffers through the reader, each checked against its generator
CREATE FUNCTION sift._test_col_wbuf_rows(index regclass, seed int8, OUT gen int8, OUT id int8, OUT deleted bool)
	RETURNS SETOF record
	AS 'MODULE_PATHNAME', 'sift_test_col_wbuf_rows' LANGUAGE C STRICT;
-- the live rows as batches (superblock, id), checked against the generator
CREATE FUNCTION sift._test_col_wbuf_batches(index regclass, seed int8, OUT gen int8, OUT sb int8, OUT id int8)
	RETURNS SETOF record
	AS 'MODULE_PATHNAME', 'sift_test_col_wbuf_batches' LANGUAGE C STRICT;
-- VACUUM's pass: flags the rows with id % modulus = 0; returns how many
CREATE FUNCTION sift._test_col_wbuf_delete(index regclass, modulus int4) RETURNS int8
	AS 'MODULE_PATHNAME', 'sift_test_col_wbuf_delete' LANGUAGE C STRICT;
CREATE FUNCTION sift._test_col_wbuf_seal(index regclass, force bool DEFAULT false) RETURNS int8
	AS 'MODULE_PATHNAME', 'sift_test_col_wbuf_seal' LANGUAGE C STRICT;
-- live: the rows sift_cwbuf_verify counts; extents: the blocks sift_cwbuf_pages reports (header included); lanes: lanes holding records
CREATE FUNCTION sift._test_col_wbuf_segments(index regclass, OUT gen int8, OUT kind text, OUT nrows int8, OUT npages int8,
											OUT live int8, OUT extents int8, OUT lanes int4)
	RETURNS SETOF record
	AS 'MODULE_PATHNAME', 'sift_test_col_wbuf_segments' LANGUAGE C STRICT;

-- Column source (src/col/test.c). preds: "leaf:op:v1|v2;..." (eq ne lt le gt ge in notin null notnull prefix); "gN:leaf:op:v" is a
-- same-element predicate of group N ("gN!" none of the elements). flags: 1 skip the write buffers, 2 skip the visibility check.
CREATE FUNCTION sift._test_col_tids(index regclass, preds text DEFAULT '', flags int4 DEFAULT 0, OUT tid tid, OUT sb int4) RETURNS SETOF record
	AS 'MODULE_PATHNAME', 'sift_test_col_tids' LANGUAGE C STRICT;
CREATE FUNCTION sift._test_col_read(index regclass, leaf int4, preds text DEFAULT '', flags int4 DEFAULT 0,
									OUT tid tid, OUT val text) RETURNS SETOF record
	AS 'MODULE_PATHNAME', 'sift_test_col_read' LANGUAGE C STRICT;
CREATE FUNCTION sift._test_col_leaf_stats(index regclass, OUT leaf int4, OUT ndistinct float8, OUT min float8, OUT max float8, OUT elems_per_row float8) RETURNS SETOF record
	AS 'MODULE_PATHNAME', 'sift_test_col_leaf_stats' LANGUAGE C STRICT;
CREATE FUNCTION sift._test_col_zones(index regclass, leaf int4, preds text DEFAULT '',
									 OUT sb int4, OUT in_wbuf bool, OUT nrows int4, OUT nvals int8, OUT nnull int8,
									 OUT min int8, OUT max int8, OUT sum_i int8, OUT exact bool) RETURNS SETOF record
	AS 'MODULE_PATHNAME', 'sift_test_col_zones' LANGUAGE C STRICT;
CREATE FUNCTION sift._test_col_types(index regclass, leaf int4, tagmask int4, deftag int4, mixed bool DEFAULT false,
									 OUT sb int4, OUT in_wbuf bool, OUT nrows int4, OUT matched int8, OUT known bool, OUT novf int8) RETURNS SETOF record
	AS 'MODULE_PATHNAME', 'sift_test_col_types' LANGUAGE C STRICT;
CREATE FUNCTION sift._test_col_formats(index regclass, OUT gen int8, OUT format int4) RETURNS SETOF record
	AS 'MODULE_PATHNAME', 'sift_test_col_formats' LANGUAGE C STRICT;
CREATE FUNCTION sift._test_col_counters(index regclass, preds text DEFAULT '', flags int4 DEFAULT 0,
										OUT rows int8, OUT segments int8, OUT wbuf_rows int8, OUT units int8,
										OUT units_pruned int8, OUT rows_scanned int8, OUT rows_vetted int8,
										OUT heap_pages int8, OUT simd text, OUT tests_made int8) RETURNS record
	AS 'MODULE_PATHNAME', 'sift_test_col_counters' LANGUAGE C STRICT;
CREATE FUNCTION sift._test_col_dict_cache(reset bool DEFAULT false, OUT loads int8, OUT hits int8, OUT bytes int8) RETURNS record
	AS 'MODULE_PATHNAME', 'sift_test_col_dict_cache' LANGUAGE C STRICT;
CREATE FUNCTION sift._test_col_pagg_fold(index regclass, dim int4, meas int4 DEFAULT -1,
										 OUT units int8, OUT answered int8, OUT groups int4, OUT rows int8, OUT total numeric) RETURNS record
	AS 'MODULE_PATHNAME', 'sift_test_col_pagg_fold' LANGUAGE C STRICT;
CREATE FUNCTION sift._test_col_cube_damage(index regclass, units int4, rows bool DEFAULT false, OUT blobs int8, OUT intact int8, OUT with_minmax int8, OUT cut int8, OUT cut_accepted int8,
										   OUT damaged int8, OUT accepted int8, OUT unsound int8, OUT census_cut_odd int8, OUT census_odd int8) RETURNS record
	AS 'MODULE_PATHNAME', 'sift_test_col_cube_damage' LANGUAGE C STRICT;
CREATE FUNCTION sift._test_col_damage(index regclass, section text, units int4, OUT objects int8, OUT intact int8, OUT cut int8, OUT cut_accepted int8,
									  OUT damaged int8, OUT accepted int8, OUT unsound int8, OUT flags int4) RETURNS record
	AS 'MODULE_PATHNAME', 'sift_test_col_damage' LANGUAGE C STRICT;
CREATE FUNCTION sift._test_col_pre_stats(index regclass, OUT leaf int4, OUT units float8, OUT dim_units float8, OUT mm_units float8, OUT roll_units float8)
	RETURNS SETOF record
	AS 'MODULE_PATHNAME', 'sift_test_col_pre_stats' LANGUAGE C STRICT;

CREATE FUNCTION sift._test_col_set_damage(index regclass, units int4, OUT blobs int8, OUT exact int8, OUT bloom int8, OUT intact int8, OUT matching int8, OUT cut int8,
										  OUT cut_accepted int8, OUT named int8, OUT named_accepted int8, OUT damaged int8, OUT accepted int8, OUT missed int8) RETURNS record
	AS 'MODULE_PATHNAME', 'sift_test_col_set_damage' LANGUAGE C STRICT;

-- Shredder (src/col/shred_test.c, docs/COL-SHRED.md)
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
CREATE FUNCTION sift._test_shred_kern_level() RETURNS text
	AS 'MODULE_PATHNAME', 'sift_test_shred_kern_level' LANGUAGE C STRICT;
CREATE FUNCTION sift._test_shred_kern_digest(seed int8, iters int4) RETURNS int8
	AS 'MODULE_PATHNAME', 'sift_test_shred_kern_digest' LANGUAGE C STRICT;
CREATE FUNCTION sift._test_shred_kern_equiv(seed int8, iters int4) RETURNS int8
	AS 'MODULE_PATHNAME', 'sift_test_shred_kern_equiv' LANGUAGE C STRICT;
CREATE FUNCTION sift._test_shred_kern_rate(kernel text, n int4, iters int4) RETURNS float8
	AS 'MODULE_PATHNAME', 'sift_test_shred_kern_rate' LANGUAGE C STRICT;

-- Column codec (src/col/codec_test.c): chunks through the backend, sift.simd and the build's LZ4/zstd.
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

-- sift_colstub (src/agg/stub*.c): an index access method without storage that carries the definition of a column index
-- (key columns, expressions, the `paths` option for jsonb columns) and a column source (src/agg/colsrc.h) that reads the heap
-- pages raw, so that the aggregate planner and executor can be tried before sift_col exists. Every row of a page is present
-- (no visibility check), the executor vets them. The index is never used by an ordinary plan.
CREATE FUNCTION sift._test_aggstub_handler(internal) RETURNS index_am_handler
	AS 'MODULE_PATHNAME', 'sift_aggstub_handler' LANGUAGE C STRICT;
CREATE ACCESS METHOD sift_colstub TYPE INDEX HANDLER sift._test_aggstub_handler;
CREATE OPERATOR CLASS sift.colstub_ops DEFAULT FOR TYPE anyelement USING sift_colstub AS STORAGE anyelement;

CREATE FUNCTION sift._test_aggstub_is_index(relam oid) RETURNS bool
	AS 'MODULE_PATHNAME', 'sift_test_aggstub_is_index' LANGUAGE C STRICT STABLE PARALLEL SAFE;
CREATE FUNCTION sift._test_aggstub_layout(index regclass, OUT leaf int4, OUT attno int4, OUT parent int4, OUT kind text,
										  OUT vtype text, OUT typid regtype, OUT path text, OUT name text, OUT expr text)
	RETURNS SETOF record
	AS 'MODULE_PATHNAME', 'sift_test_aggstub_layout' LANGUAGE C STRICT PARALLEL SAFE;
-- one row per row of the unit; for a child leaf one per element (ord = the row it belongs to, elem = its number in the unit)
CREATE FUNCTION sift._test_aggstub_dump(index regclass, leaf int4, sb_lo int4 DEFAULT 0, sb_hi int4 DEFAULT NULL,
										OUT unit int4, OUT sb int4, OUT ord int4, OUT elem int4, OUT tid tid,
										OUT code int8, OUT val text)
	RETURNS SETOF record
	AS 'MODULE_PATHNAME', 'sift_test_aggstub_dump' LANGUAGE C PARALLEL SAFE;
CREATE FUNCTION sift._test_aggstub_units(index regclass, sb_lo int4 DEFAULT 0, sb_hi int4 DEFAULT NULL,
										 prune_leaf int4 DEFAULT NULL, prune_lo int8 DEFAULT NULL, prune_hi int8 DEFAULT NULL,
										 OUT unit int4, OUT sb int4, OUT nrow int4, OUT first_tid tid, OUT last_tid tid,
										 OUT all_live bool, OUT vetted bool)
	RETURNS SETOF record
	AS 'MODULE_PATHNAME', 'sift_test_aggstub_units' LANGUAGE C PARALLEL SAFE;
CREATE FUNCTION sift._test_aggstub_dicts(index regclass, only_leaf int4 DEFAULT NULL, OUT unit int4, OUT leaf int4, OUT id text,
										 OUT n int4, OUT nbytes int4)
	RETURNS SETOF record
	AS 'MODULE_PATHNAME', 'sift_test_aggstub_dicts' LANGUAGE C PARALLEL SAFE;
CREATE FUNCTION sift._test_aggstub_counters(index regclass, sb_lo int4 DEFAULT 0, sb_hi int4 DEFAULT NULL,
											prune_leaf int4 DEFAULT NULL, prune_lo int8 DEFAULT NULL, prune_hi int8 DEFAULT NULL,
											OUT units int8, OUT units_pruned int8, OUT rows int8, OUT bytes int8)
	RETURNS record
	AS 'MODULE_PATHNAME', 'sift_test_aggstub_counters' LANGUAGE C PARALLEL SAFE;
CREATE FUNCTION sift._test_aggstub_sb(index regclass, sb int4, OUT rows int8, OUT checksum int8, OUT in_worker bool)
	RETURNS record
	AS 'MODULE_PATHNAME', 'sift_test_aggstub_sb' LANGUAGE C STRICT PARALLEL SAFE;
CREATE FUNCTION sift._test_aggstub_memory(index regclass, sb_lo int4 DEFAULT 0, sb_hi int4 DEFAULT NULL,
										  OUT units int8, OUT peak_bytes int8, OUT max_rows int8)
	RETURNS record
	AS 'MODULE_PATHNAME', 'sift_test_aggstub_memory' LANGUAGE C PARALLEL SAFE;
CREATE FUNCTION sift._test_aggstub_stats(index regclass, OUT leaf int4, OUT nrows float8, OUT nunits float8,
										 OUT allvisible_frac float8, OUT ndistinct float8, OUT min float8, OUT max float8,
										 OUT elems_per_row float8)
	RETURNS SETOF record
	AS 'MODULE_PATHNAME', 'sift_test_aggstub_stats' LANGUAGE C STRICT PARALLEL SAFE;

-- Table module (src/table/load.c): why the bulk load of a sift_table keeps the write buffers (NULL: it qualifies)
CREATE FUNCTION sift._test_tbl_bulk_refusal(tbl regclass) RETURNS text
	AS 'MODULE_PATHNAME', 'sift_test_tbl_bulk_refusal' LANGUAGE C;

-- Table module (src/table/funcs.c): takes the maintenance lock of a sift_table until the transaction ends
CREATE FUNCTION sift._test_tbl_hold_maint(tbl regclass) RETURNS void
	AS 'MODULE_PATHNAME', 'sift_test_tbl_hold_maint' LANGUAGE C STRICT;

-- Table module (src/table/funcs.c): the cube blob of the first units of a sift_table decoded as stored, cut and damaged (as _test_col_cube_damage); with_month: blobs with a month dim
CREATE FUNCTION sift._test_tbl_cube_damage(tbl regclass, units int4, OUT blobs int8, OUT intact int8, OUT with_month int8, OUT cut int8, OUT cut_accepted int8,
										   OUT damaged int8, OUT accepted int8, OUT unsound int8) RETURNS record
	AS 'MODULE_PATHNAME', 'sift_test_tbl_cube_damage' LANGUAGE C STRICT;

-- Table module (src/table/funcs.c): the bytes of the pre-aggregates of the column segments of a sift_table
CREATE FUNCTION sift._test_tbl_preagg_bytes(tbl regclass) RETURNS int8
	AS 'MODULE_PATHNAME', 'sift_test_tbl_preagg_bytes' LANGUAGE C STRICT;

-- Table module (src/table/funcs.c): the checks of sift.col_verify on every column segment of a sift_table: its live rows, an error on damage
CREATE FUNCTION sift._test_tbl_verify(tbl regclass) RETURNS int8
	AS 'MODULE_PATHNAME', 'sift_test_tbl_verify' LANGUAGE C STRICT;

-- Table module (src/table/funcs.c): the column groups of a sift_table that still carry their writer (rows that are not final)
CREATE FUNCTION sift._test_tbl_tagged(tbl regclass) RETURNS int4
	AS 'MODULE_PATHNAME', 'sift_test_tbl_tagged' LANGUAGE C STRICT;

-- the record of a write buffer page of one row whose slot and record length are given: true when it is handed out, an error when it reaches past the page
CREATE FUNCTION sift._test_tbl_page_rec(slot int4, inline_len int4) RETURNS bool
	AS 'MODULE_PATHNAME', 'sift_test_tbl_page_rec' LANGUAGE C STRICT;

-- the same for a record of inline_len bytes that ends `over` bytes past the end of the page's data (0: the largest record a page holds)
CREATE FUNCTION sift._test_tbl_page_rec_end(inline_len int4, over int4) RETURNS bool
	AS 'MODULE_PATHNAME', 'sift_test_tbl_page_rec_end' LANGUAGE C STRICT;
