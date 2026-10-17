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
#include "sift_am.h"
#include "sift_analyze.h"
#include "sift_bitmap.h"
#include "sift_build.h"
#include "sift_cpu.h"
#include "sift_kernels.h"
#include "sift_posting.h"
#include "sift_prefix.h"
#include "sift_pushdown.h"
#include "sift_sql.h"

PG_MODULE_MAGIC_EXT(.name = "pg_sift", .version = "1.0");

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

int			sift_max_expansions = 512;
bool		sift_enable_scan = true;
bool		sift_enable_count = true;
bool		sift_parallel_scan = false;
bool		sift_prefetch = true;
bool		sift_async_maintenance = true;

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
							PGC_USERSET, 0, NULL, NULL, NULL);
	DefineCustomIntVariable("sift.prefix_cache_size",
							"Memory a backend keeps the postings of broad prefixes in (kB), 0 to keep none.",
							NULL, &sift_prefix_cache_size, 8192, 0, 1048576,
							PGC_USERSET, GUC_UNIT_KB, NULL, NULL, NULL);
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
	DefineCustomBoolVariable("sift.async_maintenance",
							 "Promotes and merges segments in a dynamic background worker.",
							 NULL, &sift_async_maintenance, true,
							 PGC_SUSET, 0, NULL, NULL, NULL);
#ifdef SIFT_TESTING
	DefineCustomIntVariable("sift.grouped_min_df",
							"Testing: df from which a term's postings are GROUPED rather than LIST.",
							NULL, &sift_grouped_min_df, SIFT_GROUPED_MIN_DF, SIFT_INLINE_MAX + 1, INT_MAX,
							PGC_USERSET, GUC_NOT_IN_SAMPLE, NULL, NULL, NULL);
	DefineCustomIntVariable("sift.test_run_docs",
							"Testing: documents a CREATE INDEX run holds before it is spilled (0 = by memory).",
							NULL, &sift_test_run_docs, 0, 0, INT_MAX, PGC_USERSET, GUC_NOT_IN_SAMPLE, NULL, NULL, NULL);
	DefineCustomIntVariable("sift.test_merge_fanin",
							"Testing: runs one CREATE INDEX merge reads at a time (0 = by memory).",
							NULL, &sift_test_fanin, 0, 0, 256, PGC_USERSET, GUC_NOT_IN_SAMPLE, NULL, NULL, NULL);
	DefineCustomIntVariable("sift.test_segment_postings",
							"Testing: postings of the largest term of a CREATE INDEX segment (0 = by memory).",
							NULL, &sift_test_segment_postings, 0, 0, INT_MAX, PGC_USERSET, GUC_NOT_IN_SAMPLE, NULL, NULL, NULL);
	DefineCustomIntVariable("sift.test_blob_max",
							"Testing: bytes the postings of one term may take (0 = 4 GB).",
							NULL, &sift_test_blob_max, 0, 0, INT_MAX, PGC_USERSET, GUC_NOT_IN_SAMPLE, NULL, NULL, NULL);
	DefineCustomIntVariable("sift.test_dict_max",
							"Testing: bytes a segment's dictionary may take before its last block (0 = 4 GB).",
							NULL, &sift_test_dict_max, 0, 0, INT_MAX, PGC_USERSET, GUC_NOT_IN_SAMPLE, NULL, NULL, NULL);
	DefineCustomIntVariable("sift.test_build_peak",
							"Testing: kB of memory the CREATE INDEX of this session allocated at most (set 0 before).",
							NULL, &sift_test_build_peak, 0, 0, INT_MAX, PGC_USERSET, GUC_NOT_IN_SAMPLE | GUC_UNIT_KB, NULL, NULL, NULL);
#endif
	sift_pushdown_init();
	MarkGUCPrefixReserved("sift");
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
	define_gucs();
	define_reloptions();
	sift_planner_init();
#ifdef SIFT_TESTING
	sift_leak_test_init();
#endif
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
		{"build_segments", RELOPT_TYPE_INT, offsetof(SiftOptions, build_segments)},
		{"prefix_index", RELOPT_TYPE_INT, offsetof(SiftOptions, prefix_index)},
		{"fields", RELOPT_TYPE_STRING, offsetof(SiftOptions, fields)},
		{"analyzer", RELOPT_TYPE_ENUM, offsetof(SiftOptions, analyzer)},
		{"positions", RELOPT_TYPE_BOOL, offsetof(SiftOptions, positions)},
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
		.positions = true,
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

Datum
sift_handler(PG_FUNCTION_ARGS)
{
	IndexAmRoutine *am = makeNode(IndexAmRoutine);

	am->amstrategies = 1;
	am->amsupport = 0;
	am->amoptsprocnum = 0;
	am->amcanorder = false;
	am->amcanorderbyop = false;
	am->amcanbackward = false;
	am->amcanunique = false;
	am->amcanmulticol = false;
	am->amoptionalkey = false;
	am->amsearcharray = false;
	am->amsearchnulls = false;
	am->amstorage = false;
	am->amclusterable = false;
	am->ampredlocks = false;
	am->amcanparallel = false;
	am->amcanbuildparallel = true;
	am->amcaninclude = false;
	am->amusemaintenanceworkmem = true;
	am->amsummarizing = false;
	am->amparallelvacuumoptions = VACUUM_OPTION_PARALLEL_BULKDEL;
	am->amkeytype = InvalidOid;

	am->ambuild = sift_ambuild;
	am->ambuildempty = sift_ambuildempty;
	am->aminsert = sift_aminsert;
	am->aminsertcleanup = NULL;
	am->ambulkdelete = sift_ambulkdelete;
	am->amvacuumcleanup = sift_amvacuumcleanup;
	am->amcanreturn = NULL;
	am->amcostestimate = sift_amcostestimate;
	am->amoptions = sift_amoptions;
	am->amproperty = NULL;
	am->ambuildphasename = NULL;
	am->amvalidate = sift_amvalidate;
	am->amadjustmembers = NULL;
	am->ambeginscan = sift_ambeginscan;
	am->amrescan = sift_amrescan;
	am->amgettuple = NULL;
	am->amgetbitmap = sift_amgetbitmap;
	am->amendscan = sift_amendscan;
	am->ammarkpos = NULL;
	am->amrestrpos = NULL;
	am->amestimateparallelscan = NULL;
	am->aminitparallelscan = NULL;
	am->amparallelrescan = NULL;
	SIFT_AM_NEW_FIELDS(am);

	PG_RETURN_POINTER(am);
}
