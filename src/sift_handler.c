/*
 * sift_handler.c - module init, GUCs, reloptions, catalog cache and the
 * index AM handler. All PG_FUNCTION_INFO_V1 declarations live here.
 */
#include "postgres.h"

#include "access/amapi.h"
#include "access/reloptions.h"
#include "catalog/namespace.h"
#include "catalog/pg_type_d.h"
#include "commands/defrem.h"
#include "commands/vacuum.h"
#include "fmgr.h"
#include "nodes/value.h"
#include "parser/parse_func.h"
#include "parser/parse_oper.h"
#include "utils/guc.h"
#include "utils/inval.h"
#include "utils/rel.h"
#include "utils/syscache.h"

#include "sift.h"
#include "sift_agg.h"
#include "sift_aggstub.h"
#include "sift_am.h"
#include "sift_analyze.h"
#include "sift_bitmap.h"
#include "sift_build.h"
#include "sift_col_am.h"
#include "sift_col_kern.h"
#include "sift_colcodec.h"
#include "sift_cpu.h"
#include "sift_kernels.h"
#include "sift_merge.h"
#include "sift_ops.h"
#include "sift_posting.h"
#include "sift_prefix.h"
#include "sift_table.h"
#include "sift_segment.h"
#include "sift_pushdown.h"
#include "sift_shred.h"
#include "sift_sql.h"

PG_MODULE_MAGIC_EXT(.name = "pg_sift", .version = SIFT_MODULE_VERSION);

PG_FUNCTION_INFO_V1(sift_handler);
PG_FUNCTION_INFO_V1(sift_match);
PG_FUNCTION_INFO_V1(sift_match_idx);
PG_FUNCTION_INFO_V1(sift_match_support);
PG_FUNCTION_INFO_V1(sift_matchsel);
PG_FUNCTION_INFO_V1(sift_score);
PG_FUNCTION_INFO_V1(sift_max_score);
PG_FUNCTION_INFO_V1(sift_highlight);
PG_FUNCTION_INFO_V1(sift_tokenize);
PG_FUNCTION_INFO_V1(sift_parse_query);
PG_FUNCTION_INFO_V1(sift_promote);
PG_FUNCTION_INFO_V1(sift_merge);
PG_FUNCTION_INFO_V1(sift_segments);
PG_FUNCTION_INFO_V1(sift_segment_postings);
PG_FUNCTION_INFO_V1(sift_verify);
PG_FUNCTION_INFO_V1(sift_facet_counts);
PG_FUNCTION_INFO_V1(sift_version);
PG_FUNCTION_INFO_V1(sift_index_info);
PG_FUNCTION_INFO_V1(sift_col_handler);
PG_FUNCTION_INFO_V1(sift_col_index_info);
PG_FUNCTION_INFO_V1(sift_col_segments);
PG_FUNCTION_INFO_V1(sift_col_leaves);
PG_FUNCTION_INFO_V1(sift_col_sections);
PG_FUNCTION_INFO_V1(sift_col_verify);
PG_FUNCTION_INFO_V1(sift_col_promote);
PG_FUNCTION_INFO_V1(sift_col_merge);

int			sift_max_expansions = 512;
bool		sift_enable_scan = true;
bool		sift_enable_count = true;
bool		sift_parallel_scan = false;
bool		sift_prefetch = true;
bool		sift_async_maintenance = true;
int			sift_maintenance_naptime = 2000;
int			sift_maintenance_cost_delay = 0;

static relopt_kind sift_relopt_kind;
static SiftCatalog catalog;
static bool catalog_valid;

static void
define_gucs(void)
{
	DefineCustomIntVariable("sift.max_expansions",
							"Maximum terms a wildcard, regex or fuzzy leaf may expand to, and a prefix may be scored over.",
							"A prefix with more terms matches all of them and scores as the one term prefix_index would index.",
							&sift_max_expansions, 512, 1, 1000000,
							PGC_SUSET, 0, NULL, NULL, NULL);
	DefineCustomIntVariable("sift.prefix_cache_size",
							"Memory a backend keeps the postings of broad prefixes in (kB), 0 to keep none.",
							NULL, &sift_prefix_cache_size, 8192, 0, 1048576,
							PGC_SUSET, GUC_UNIT_KB, NULL, NULL, NULL);
	DefineCustomBoolVariable("sift.enable_scan",
							 "Enables the sift custom scan (ranked and unordered).",
							 NULL, &sift_enable_scan, true,
							 PGC_USERSET, 0, NULL, NULL, NULL);
	DefineCustomBoolVariable("sift.enable_count",
							 "Enables count(*) pushdown into the sift index.",
							 NULL, &sift_enable_count, true,
							 PGC_USERSET, 0, NULL, NULL, NULL);
	DefineCustomBoolVariable("sift.parallel_scan",
							 "Lets the planner use parallel sift scans and counts (they pay off on large, heap-bound scans only).",
							 NULL, &sift_parallel_scan, false,
							 PGC_USERSET, 0, NULL, NULL, NULL);
	DefineCustomBoolVariable("sift.prefetch",
							 "Reads ahead the heap pages an unordered scan is about to visit.",
							 NULL, &sift_prefetch, true,
							 PGC_USERSET, 0, NULL, NULL, NULL);
	DefineCustomBoolVariable("sift.standby_conflicts",
							 "Logs the reuse of released blocks as a recovery conflict, so a standby without hot_standby_feedback waits for or cancels the queries that may still read them.",
							 "Off, such a query fails with \"segment recycled during scan, retry\" when the blocks it reads are reused.",
							 &sift_standby_conflicts, false,
							 PGC_SUSET, 0, NULL, NULL, NULL);
	DefineCustomBoolVariable("sift.async_maintenance",
							 "Promotes and merges segments in a dynamic background worker.",
							 NULL, &sift_async_maintenance, true,
							 PGC_SUSET, 0, NULL, NULL, NULL);
	DefineCustomIntVariable("sift.maintenance_naptime",
							"Time between background maintenance workers of an index; a worker stays this long after it starts.",
							NULL, &sift_maintenance_naptime, 2000, 200, 60000,
							PGC_SIGHUP, GUC_UNIT_MS, NULL, NULL, NULL);
	DefineCustomIntVariable("sift.maintenance_cost_delay",
							"Milliseconds the maintenance worker sleeps after every 16 pages (128 kB) its merges read or write, 0 to run them at full speed.",
							NULL, &sift_maintenance_cost_delay, 0, 0, 100,
							PGC_SUSET, GUC_UNIT_MS, NULL, NULL, NULL);
#ifdef SIFT_TESTING
	DefineCustomIntVariable("sift.grouped_min_df",
							"Testing: df from which a term's postings are GROUPED rather than LIST.",
							NULL, &sift_grouped_min_df, SIFT_GROUPED_MIN_DF, SIFT_INLINE_MAX + 1, INT_MAX,
							PGC_USERSET, GUC_NOT_IN_SAMPLE, NULL, NULL, NULL);
	DefineCustomIntVariable("sift.test_prefix_gather",
							"Testing: kB of postings one pass of a wide prefix gathers (the rest is built in windows).",
							NULL, &sift_prefix_gather_kb, 131072, 1, INT_MAX, PGC_USERSET, GUC_UNIT_KB | GUC_NOT_IN_SAMPLE, NULL, NULL, NULL);
	DefineCustomIntVariable("sift.test_docdir_min_sb",
							"Testing: superblocks from which a segment's docmap directory is kept for the next query.",
							NULL, &sift_docdir_min_sb, 128, 1, INT_MAX, PGC_USERSET, GUC_NOT_IN_SAMPLE, NULL, NULL, NULL);
	DefineCustomIntVariable("sift.test_run_docs",
							"Testing: documents a CREATE INDEX run holds before it is spilled (0 = by memory).",
							NULL, &sift_test_run_docs, 0, 0, INT_MAX, PGC_USERSET, GUC_NOT_IN_SAMPLE, NULL, NULL, NULL);
	DefineCustomIntVariable("sift.test_merge_fanin",
							"Testing: runs one CREATE INDEX merge reads at a time (0 = by memory).",
							NULL, &sift_test_fanin, 0, 0, 256, PGC_USERSET, GUC_NOT_IN_SAMPLE, NULL, NULL, NULL);
	DefineCustomIntVariable("sift.test_segment_postings",
							"Testing: postings of the largest term of a CREATE INDEX segment (0 = by memory).",
							NULL, &sift_test_segment_postings, 0, 0, INT_MAX, PGC_USERSET, GUC_NOT_IN_SAMPLE, NULL, NULL, NULL);
	sift_store_test_init();
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
	DefineCustomIntVariable("sift.test_phase_gate",
							"Testing: a CREATE INDEX waits to enter each phase for the advisory lock this number plus the phase (0 = no gate).",
							NULL, &sift_test_phase_gate, 0, 0, INT_MAX, PGC_USERSET, GUC_NOT_IN_SAMPLE, NULL, NULL, NULL);
	DefineCustomIntVariable("sift.test_build_peak",
							"Testing: kB of memory the CREATE INDEX of this session allocated at most (set 0 before).",
							NULL, &sift_test_build_peak, 0, 0, INT_MAX, PGC_USERSET, GUC_NOT_IN_SAMPLE | GUC_UNIT_KB, NULL, NULL, NULL);
#endif
	sift_pushdown_init();
	sift_ops_init();
}

static relopt_enum_elt_def analyzer_names[] = {
	{"text", SIFT_ANALYZER_TEXT},
	{"keyword", SIFT_ANALYZER_KEYWORD},
	{(const char *) NULL}
};

static void
define_reloptions(void)
{
	sift_relopt_kind = add_reloption_kind();
	add_real_reloption(sift_relopt_kind, "k1", "BM25 term frequency saturation",
					   SIFT_DEFAULT_K1, 0.0, 1000.0, AccessExclusiveLock);
	add_real_reloption(sift_relopt_kind, "b", "BM25 document length normalization",
					   SIFT_DEFAULT_B, 0.0, 1.0, AccessExclusiveLock);
	add_string_reloption(sift_relopt_kind, "stemmer",
						 "Stemmer: en (Snowball English) or none",
						 "", sift_analyze_validate_stemmer, AccessExclusiveLock);
	add_bool_reloption(sift_relopt_kind, "case_folding", "Fold case",
					   true, AccessExclusiveLock);
	add_bool_reloption(sift_relopt_kind, "accent_folding", "Strip accents",
					   true, AccessExclusiveLock);
	add_int_reloption(sift_relopt_kind, "mutable_limit",
					  "Mutable segment size in kB before it is sealed for promotion",
					  SIFT_DEFAULT_MUTABLE_LIMIT, 64, 1048576, ShareUpdateExclusiveLock);
	add_int_reloption(sift_relopt_kind, "vacuum_segments",
					  "Directory entries (segments, the mutable one included) VACUUM merges the index down to; more keep VACUUM short and queries slower",
					  SIFT_DEFAULT_VACUUM_SEGMENTS, 1, SIFT_MAX_SEGMENTS, ShareUpdateExclusiveLock);
	add_int_reloption(sift_relopt_kind, "merge_fanin",
					  "Segments of a size tier that trigger a merge; a lower value merges more often and keeps fewer segments, a higher one merges less",
					  SIFT_MERGE_FANIN, SIFT_MERGE_MIN_FANIN, SIFT_MERGE_MAX_FANIN, ShareUpdateExclusiveLock);
	add_int_reloption(sift_relopt_kind, "build_segments",
					  "Segments written by CREATE INDEX, 0 for one per build participant",
					  0, 0, SIFT_MAX_SEGMENTS / 2, AccessExclusiveLock);
	add_bool_reloption(sift_relopt_kind, "positions",
					   "Store word positions, so phrase queries are decided by the index instead of rechecked in the heap (REINDEX to apply)",
					   true, AccessExclusiveLock);
	add_string_reloption(sift_relopt_kind, "fields",
						 "Names of the first sections of the indexed text (split at chr(29)) that queries can search apart, as name:word",
						 "", sift_analyze_validate_fields, AccessExclusiveLock);
	add_int_reloption(sift_relopt_kind, "prefix_index",
					  "Index the first 1..n characters of every word, so prefix queries of up to n characters need no expansion (0 = off, REINDEX to apply)",
					  0, 0, SIFT_MAX_PREFIX_INDEX, AccessExclusiveLock);
	add_enum_reloption(sift_relopt_kind, "analyzer",
					   "Analyzer: text, or keyword to index each whole value as one term (facets, REINDEX to apply)",
					   analyzer_names, SIFT_ANALYZER_TEXT,
					   "Valid values are \"text\" and \"keyword\".", AccessExclusiveLock);
	add_int_reloption(sift_relopt_kind, "dead_rewrite_pct",
					  "Percent of dead documents above which maintenance rewrites a lone segment to drop them (100 never)",
					  SIFT_MERGE_DEAD_PCT, 1, 100, ShareUpdateExclusiveLock);
	add_bool_reloption(sift_relopt_kind, "vacuum_truncate",
					   "VACUUM gives the free blocks at the end of the index file back to the operating system (takes a brief exclusive lock when it can)",
					   true, ShareUpdateExclusiveLock);
}

/* Binds the codec's own ISA dispatch to the sift.simd level */
static void
codec_rebind(const SiftCpu *cpu)
{
	static const int isa[] = {SIFT_ISA_SCALAR, SIFT_ISA_AVX2, SIFT_ISA_AVX512};

	for (int level = cpu->active; !sift_set_isa(isa[level]); level--)
		;
}

static void
catalog_invalidate(Datum arg, int cacheid, uint32 hashvalue)
{
	catalog_valid = false;
}

void
_PG_init(void)
{
	sift_cpu_init();
	sift_init();
	sift_cpu_register(codec_rebind);
	sift_kernels_init();
	sift_col_kernels_init();
	sift_col_init();
	sift_shred_init();
	sift_cc_kernels_init();
	define_gucs();
	define_reloptions();
	sift_planner_init();
	sift_agg_init();
	sift_tbl_init();
#ifdef SIFT_TESTING
	sift_leak_test_init();
	sift_aggstub_init();
#endif
	/* after the last GUC is defined: a value set before the library loaded is applied to its GUC, not dropped with the placeholder */
	MarkGUCPrefixReserved("sift");
	CacheRegisterSyscacheCallback(NAMESPACEOID, catalog_invalidate, (Datum) 0);
	CacheRegisterSyscacheCallback(PROCOID, catalog_invalidate, (Datum) 0);
	CacheRegisterSyscacheCallback(OPEROID, catalog_invalidate, (Datum) 0);
}

static Oid
sift_func(const char *name, int nargs, const Oid *argtypes)
{
	return LookupFuncName(list_make2(makeString("sift"), makeString(pstrdup(name))),
						  nargs, argtypes, true);
}

const SiftCatalog *
sift_catalog(void)
{
	static const Oid text2[] = {TEXTOID, TEXTOID};
	static const Oid text2oid[] = {TEXTOID, TEXTOID, OIDOID};
	static const Oid tid1[] = {TIDOID};

	if (catalog_valid)
		return &catalog;
	memset(&catalog, 0, sizeof(catalog));
	if (OidIsValid(get_namespace_oid("sift", true)))
	{
		catalog.am = get_index_am_oid("sift", true);
		catalog.match_op = OpernameGetOprid(list_make2(makeString("pg_catalog"), makeString("@@>")),
											TEXTOID, TEXTOID);
		catalog.match_func = sift_func("match", 2, text2);
		catalog.match_idx_func = sift_func("match_idx", 3, text2oid);
		catalog.score_func = sift_func("score", 1, tid1);
		catalog.max_score_func = sift_func("max_score", 1, tid1);
	}
	catalog_valid = true;
	return &catalog;
}

static bytea *
sift_amoptions(Datum reloptions, bool validate)
{
	static const relopt_parse_elt tab[] = {
		{"k1", RELOPT_TYPE_REAL, offsetof(SiftOptions, k1)},
		{"b", RELOPT_TYPE_REAL, offsetof(SiftOptions, b)},
		{"stemmer", RELOPT_TYPE_STRING, offsetof(SiftOptions, stemmer)},
		{"case_folding", RELOPT_TYPE_BOOL, offsetof(SiftOptions, case_folding)},
		{"accent_folding", RELOPT_TYPE_BOOL, offsetof(SiftOptions, accent_folding)},
		{"mutable_limit", RELOPT_TYPE_INT, offsetof(SiftOptions, mutable_limit)},
		{"vacuum_segments", RELOPT_TYPE_INT, offsetof(SiftOptions, vacuum_segments)},
		{"merge_fanin", RELOPT_TYPE_INT, offsetof(SiftOptions, merge_fanin)},
		{"build_segments", RELOPT_TYPE_INT, offsetof(SiftOptions, build_segments)},
		{"prefix_index", RELOPT_TYPE_INT, offsetof(SiftOptions, prefix_index)},
		{"fields", RELOPT_TYPE_STRING, offsetof(SiftOptions, fields)},
		{"analyzer", RELOPT_TYPE_ENUM, offsetof(SiftOptions, analyzer)},
		{"positions", RELOPT_TYPE_BOOL, offsetof(SiftOptions, positions)},
		{"dead_rewrite_pct", RELOPT_TYPE_INT, offsetof(SiftOptions, dead_rewrite_pct)},
		{"vacuum_truncate", RELOPT_TYPE_BOOL, offsetof(SiftOptions, vacuum_truncate)},
	};

	return (bytea *) build_reloptions(reloptions, validate, sift_relopt_kind,
									  sizeof(SiftOptions), tab, lengthof(tab));
}

const SiftOptions *
sift_index_options(Relation index)
{
	static SiftOptions defaults = {
		.k1 = SIFT_DEFAULT_K1,
		.b = SIFT_DEFAULT_B,
		.case_folding = true,
		.accent_folding = true,
		.mutable_limit = SIFT_DEFAULT_MUTABLE_LIMIT,
		.vacuum_segments = SIFT_DEFAULT_VACUUM_SEGMENTS,
		.merge_fanin = SIFT_MERGE_FANIN,
		.positions = true,
		.dead_rewrite_pct = SIFT_MERGE_DEAD_PCT,
		.vacuum_truncate = true,
	};

	return index->rd_options ? (const SiftOptions *) index->rd_options : &defaults;
}

const char *
sift_options_stemmer(const SiftOptions *opts)
{
	return opts->stemmer ? (const char *) opts + opts->stemmer : "";
}

const char *
sift_options_fields(const SiftOptions *opts)
{
	return opts->fields ? (const char *) opts + opts->fields : "";
}

static bool
sift_amvalidate(Oid opclassoid)
{
	return true;
}

/* What the sift access methods share: bitmap scans only, no ordering or uniqueness, a parallel build and parallel bulk delete (makeNode zeroes the rest) */
IndexAmRoutine *
sift_am_routine(int strategies, int support, bool multicol)
{
	IndexAmRoutine *am = makeNode(IndexAmRoutine);

	am->amstrategies = strategies;
	am->amsupport = support;
	am->amcanmulticol = am->amoptionalkey = multicol;
	am->amcanbuildparallel = am->amusemaintenanceworkmem = true;
	am->amparallelvacuumoptions = VACUUM_OPTION_PARALLEL_BULKDEL;
	am->amkeytype = InvalidOid;
	return am;
}

Datum
sift_handler(PG_FUNCTION_ARGS)
{
	IndexAmRoutine *am = sift_am_routine(1, 0, false);

	am->ambuild = sift_ambuild;
	am->ambuildempty = sift_ambuildempty;
	am->aminsert = sift_aminsert;
	am->ambulkdelete = sift_ambulkdelete;
	am->amvacuumcleanup = sift_amvacuumcleanup;
	am->amcostestimate = sift_amcostestimate;
	am->amoptions = sift_amoptions;
	am->ambuildphasename = sift_ambuildphasename;
	am->amvalidate = sift_amvalidate;
	am->ambeginscan = sift_ambeginscan;
	am->amrescan = sift_amrescan;
	am->amgetbitmap = sift_amgetbitmap;
	am->amendscan = sift_amendscan;
	PG_RETURN_POINTER(am);
}
