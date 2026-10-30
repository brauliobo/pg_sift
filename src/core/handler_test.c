/*
 * core/handler_test.c - the GUCs of the test build: the knobs a test turns to make a build, a merge, a scan or a space move stop, wait, fail or run in small steps
 * compiled only with SIFT_TESTING; sift_handler_test_gucs runs from define_gucs (core/handler.c) in the place the definitions stood
 */
#include "postgres.h"

#ifdef SIFT_TESTING

#include "bm25/build.h"
#include "bm25/prefix.h"
#include "core/visibility.h"
#include "core/parbuild.h"

void
sift_handler_test_gucs(void)
{
	DefineCustomIntVariable("sift.grouped_min_df",
							"Testing: df from which a term's postings are GROUPED rather than LIST.",
							NULL, &sift_grouped_min_df, SIFT_GROUPED_MIN_DF, SIFT_INLINE_MAX + 1, INT_MAX,
							PGC_USERSET, GUC_NOT_IN_SAMPLE, NULL, NULL, NULL);
	DefineCustomIntVariable("sift.test_segfmt",
							"Testing: segment format a build, promotion or merge writes (0 = the newest, 1 to 3).",
							NULL, &sift_test_segfmt, 0, 0, SIFT_SEGFMT_PACKED, PGC_USERSET, GUC_NOT_IN_SAMPLE, NULL, NULL, NULL);
	DefineCustomIntVariable("sift.test_prefix_gather",
							"Testing: kB of postings one pass of a wide prefix gathers (the rest is built in windows).",
							NULL, &sift_prefix_gather_kb, 131072, 1, INT_MAX, PGC_USERSET, GUC_UNIT_KB | GUC_NOT_IN_SAMPLE, NULL, NULL, NULL);
	DefineCustomIntVariable("sift.test_docdir_min_sb",
							"Testing: superblocks from which a segment's docmap directory is kept for the next query.",
							NULL, &sift_docdir_min_sb, 128, 1, INT_MAX, PGC_USERSET, GUC_NOT_IN_SAMPLE, NULL, NULL, NULL);
	DefineCustomIntVariable("sift.test_run_docs",
							"Testing: documents a CREATE INDEX run holds before it is spilled (0 = by memory).",
							NULL, &sift_test_run_docs, 0, 0, INT_MAX, PGC_USERSET, GUC_NOT_IN_SAMPLE, NULL, NULL, NULL);
	DefineCustomIntVariable("sift.test_par_workers",
							"Testing: parallel workers the last CREATE INDEX of this session launched (a test sets it to 0 before the build).",
							NULL, &sift_test_par_workers, 0, 0, INT_MAX, PGC_USERSET, GUC_NOT_IN_SAMPLE, NULL, NULL, NULL);
	DefineCustomIntVariable("sift.test_merge_fanin",
							"Testing: runs one CREATE INDEX merge reads at a time (0 = by memory).",
							NULL, &sift_test_fanin, 0, 0, 256, PGC_USERSET, GUC_NOT_IN_SAMPLE, NULL, NULL, NULL);
	DefineCustomIntVariable("sift.test_segment_postings",
							"Testing: postings of the largest term of a CREATE INDEX segment (0 = by memory).",
							NULL, &sift_test_segment_postings, 0, 0, INT_MAX, PGC_USERSET, GUC_NOT_IN_SAMPLE, NULL, NULL, NULL);
	sift_store_test_init();
	DefineCustomIntVariable("sift.test_vis_pause",
							"Testing: the Nth visibility-map read of a scan waits for advisory lock (0x51F7, 1) first (0 = none).",
							NULL, &sift_test_vis_pause, 0, 0, INT_MAX, PGC_USERSET, GUC_NOT_IN_SAMPLE, NULL, NULL, NULL);
	DefineCustomBoolVariable("sift.test_count_gate",
							 "Testing: a count waits for advisory lock (0x51F7, 1) once its plan is made.",
							 NULL, &sift_test_count_gate, false, PGC_USERSET, GUC_NOT_IN_SAMPLE, NULL, NULL, NULL);
	DefineCustomBoolVariable("sift.test_cleanup_gate",
							 "Testing: a VACUUM waits for advisory lock (0x51F7, 2) before its cleanup refreshes the dead-posting tables.",
							 NULL, &sift_test_cleanup_gate, false, PGC_USERSET, GUC_NOT_IN_SAMPLE, NULL, NULL, NULL);
	DefineCustomBoolVariable("sift.test_ahead_cold",
							 "Testing: a segment reader reads ahead from its first read, as one whose pages are not in shared buffers does.",
							 NULL, &sift_test_ahead_cold, false, PGC_USERSET, GUC_NOT_IN_SAMPLE, NULL, NULL, NULL);
	DefineCustomIntVariable("sift.test_vis_jitter_us",
							"Testing: a scan sleeps up to this many microseconds before each visibility-map read (0 = no).",
							NULL, &sift_test_vis_jitter_us, 0, 0, 1000000, PGC_USERSET, GUC_NOT_IN_SAMPLE, NULL, NULL, NULL);
	DefineCustomIntVariable("sift.test_blob_max",
							"Testing: bytes the postings of one term may take (0 = 4 GB).",
							NULL, &sift_test_blob_max, 0, 0, INT_MAX, PGC_USERSET, GUC_NOT_IN_SAMPLE, NULL, NULL, NULL);
	DefineCustomIntVariable("sift.test_dict_max",
							"Testing: bytes a segment's dictionary may take before its last block (0 = 4 GB).",
							NULL, &sift_test_dict_max, 0, 0, INT_MAX, PGC_USERSET, GUC_NOT_IN_SAMPLE, NULL, NULL, NULL);
	DefineCustomIntVariable("sift.test_pool_blocks",
							"Testing: 64 kB blocks of the CREATE INDEX buffer's pool (0 = 60000).",
							NULL, &sift_test_pool_blocks, 0, 0, 60000, PGC_USERSET, GUC_NOT_IN_SAMPLE, NULL, NULL, NULL);
	DefineCustomIntVariable("sift.test_page_text",
							"Testing: bytes of the text of a heap page CREATE INDEX buffers (0 = by memory).",
							NULL, &sift_test_page_text, 0, 0, INT_MAX, PGC_USERSET, GUC_NOT_IN_SAMPLE, NULL, NULL, NULL);
	DefineCustomIntVariable("sift.test_fail_merge",
							"Testing: the nth merge of a CREATE INDEX is cancelled after a few terms (0 = never).",
							NULL, &sift_test_fail_merge, 0, 0, INT_MAX, PGC_USERSET, GUC_NOT_IN_SAMPLE, NULL, NULL, NULL);
	DefineCustomIntVariable("sift.test_fail_who",
							"Testing: sift.test_fail_merge in the leader or a serial build (1), the workers (2), any (0).",
							NULL, &sift_test_fail_who, 0, 0, 2, PGC_USERSET, GUC_NOT_IN_SAMPLE, NULL, NULL, NULL);
	DefineCustomBoolVariable("sift.test_space_error",
							 "Testing: error after publishing an open write buffer relocation.",
							 NULL, &sift_test_space_error, false, PGC_USERSET, GUC_NOT_IN_SAMPLE, NULL, NULL, NULL);
	DefineCustomIntVariable("sift.test_space_crash",
							"Testing: kill the backend after recycling (1), relocation publication (2), lowering high_water (3), or the publication of a shift (4).",
							NULL, &sift_test_space_crash, 0, 0, 4, PGC_USERSET, GUC_NOT_IN_SAMPLE, NULL, NULL, NULL);
	DefineCustomIntVariable("sift.test_phase_gate",
							"Testing: a CREATE INDEX waits to enter each phase for the advisory lock this number plus the phase (0 = no gate).",
							NULL, &sift_test_phase_gate, 0, 0, INT_MAX, PGC_USERSET, GUC_NOT_IN_SAMPLE, NULL, NULL, NULL);
	DefineCustomIntVariable("sift.test_build_peak",
							"Testing: kB of memory the CREATE INDEX of this session allocated at most (set 0 before).",
							NULL, &sift_test_build_peak, 0, 0, INT_MAX, PGC_USERSET, GUC_NOT_IN_SAMPLE | GUC_UNIT_KB, NULL, NULL, NULL);
}

#endif							/* SIFT_TESTING */
