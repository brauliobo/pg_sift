/*
 * col/v1/store_ops_zone.c - SiftZoneOps and SiftDictOps of Format 1: the aggregate scan of a sift_col index or a sift_table of Format 1 (col/source_fold.h folds the batches of its source into the
 * groups of SiftAggV6), and the predicates of the statement's quals a sift_table decides. The source skips the units whose zone of an integer column lies outside the bounds of the quals, vets the rows
 * against the tombstones and the snapshot, and decides the quals on the rows of the batches; the write buffers are units like the others. A text leaf is given as SVEC_STRVIEW: Format 1 keeps no
 * dictionary a batch aggregates by its codes, so it has none to offer (has_dict32 is false).
 */
#include "postgres.h"

#include "access/stratnum.h"
#include "catalog/pg_type_d.h"
#include "executor/executor.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"

#include "agg/engine.h"
#include "agg/ext.h"
#include "col/am.h"
#include "col/source_fold.h"
#include "col/v1/point.h"
#include "col/v1/store_ops.h"
#include "core/memtrack.h"
#include "core/store/ops.h"

typedef struct V1Open
{
	MemoryContext cx;
	Snapshot	snap;
	const SiftV1Scan *scan;
	const SiftV1UnitSink *cube;
} V1Open;

static SiftV6Source *
open_source(void *arg, const SiftV6Input *in, const SiftV6Vexpr *quals, const struct SiftV6Bq *texts)
{
	const V1Open *o = arg;
	SiftV1Scan	scan = o->scan != NULL ? *o->scan : (SiftV1Scan) {0};

	scan.cube = o->cube;
	scan.texts = texts;
	return sift_v1_source_open(o->cx, o->snap, in, quals, &scan);
}

static struct SiftStoreQuals *
zone_compile_quals(Relation heap, List *indexqual, List *jsonqual, MemoryContext cx)
{
	MemoryContext old = MemoryContextSwitchTo(cx);
	struct SiftStoreQuals *q = palloc0(sizeof(struct SiftStoreQuals));

	Assert(jsonqual == NIL);

	q->clauses = sift_srcfold_heap_quals(heap, indexqual);
	MemoryContextSwitchTo(old);
	return q;
}

static void
zone_bind_quals(Relation heap, struct SiftStoreQuals *q, ExprContext *econtext)
{
	q->estate = econtext->ecxt_estate;
}

/* a sift_table is not read as an index scan: its quals are no scan keys */
static bool
zone_quals_keys(const struct SiftStoreQuals *q, ScanKey *keys, int *nkeys)
{
	return false;
}

SiftV1Scan
sift_v1_spec_scan(const SiftV1Scan *scan, const struct SiftV6Spec *spec)
{
	SiftV1Scan	out = scan != NULL ? *scan : (SiftV1Scan) {0};

	out.tid_range = spec->tid_range;
	out.tid_lo = spec->tid_lo;
	out.tid_hi = spec->tid_hi;
	return out;
}

/* One fold for the whole scan: its pipe keeps which rows of the dimensions matched across the chunks, and gives the rows no chunk matched once, at the end */
static void
fold_run(const SiftSrcFoldArgs *a, Snapshot snap, const SiftV1Scan *keys, bool (*next_chunk) (void *arg, const struct SiftTidBridge **bridge), void *chunk_arg)
{
	MemoryContext cx = sift_mem_allocset_create(CurrentMemoryContext, "sift v1 zone scan");
	SiftV1Scan	scan = sift_v1_spec_scan(keys, a->spec);
	V1Open		o = {.cx = cx,.snap = snap,.scan = &scan,.cube = next_chunk == NULL ?
		sift_v1_range_count_begin(a, &scan, cx, sift_v1_dict_count_begin(a, &scan, cx, sift_v1_ktab_begin(a, cx, sift_v1_cube_begin(a, &scan, cx)))) : NULL};
	SiftSrcFold *f = sift_srcfold_new(a, sift_mem_allocset_create(cx, "sift v1 zone fold"));
	const struct SiftTidBridge *bridge = NULL;

	if (next_chunk == NULL)
		sift_srcfold_run(f, open_source, &o);
	while (next_chunk != NULL && next_chunk(chunk_arg, &bridge))
	{
		sift_srcfold_count_chunk(a->stats, bridge);
		scan.bridge = bridge;
		sift_srcfold_run(f, open_source, &o);
	}
	sift_srcfold_finish(f);
	MemoryContextDelete(cx);
}

static void
zone_scan_table(const struct SiftV6Spec *spec, struct SiftV6Group *g, Relation rel, const struct SiftStoreQuals *quals, Snapshot snap, const uint64 *skip, int nskip,
				struct SiftV6ScanStats *stats, struct SiftV6StarRun *star,
				bool (*next_chunk) (void *arg, const struct SiftTidBridge **bridge), void *chunk_arg)
{
	SIFT_INVARIANT(nskip == 0, "Format 1 has no device parts to leave out of the scan of table %u", RelationGetRelid(rel));
	SIFT_INVARIANT(next_chunk == NULL, "Format 1 tables are not scanned by the hits of a text query (table %u)", RelationGetRelid(rel));
	fold_run(&(SiftSrcFoldArgs) {.spec = spec,.group = g,.stats = stats,.star = star,.host = rel,.is_table = true,.quals = quals->clauses,.estate = quals->estate}, snap, NULL, NULL, NULL);
}

/* `column = 'text'` or `column = ANY ('{..}'::text[])` of a top-level text leaf, under a collation that compares bytes: its strings, as the dictionaries of a segment hold them */
static bool
str_key(Relation index, const SiftSrcLayout *lay, const ScanKeyData *k, SiftSrcPrune *out)
{
	const SiftSrcLeaf *l = sift_v1_att_leaf(lay, k->sk_attno);
	Datum		one = k->sk_argument;
	Datum	   *vals = &one;
	bool	   *nulls;
	int			n;

	if (l == NULL || l->vtype != SCV_CODE || l->wide || l->lossy || l->astext || l->parent >= 0 || l->path != NIL || k->sk_strategy != BTEqualStrategyNumber ||
		k->sk_subtype != TEXTOID || (k->sk_flags & SK_ISNULL) || !OidIsValid(k->sk_collation) || !get_collation_isdeterministic(k->sk_collation))
		return false;
	if (k->sk_flags & SK_SEARCHARRAY)
		deconstruct_array_builtin(DatumGetArrayTypeP(k->sk_argument), TEXTOID, &vals, &nulls, &n);
	else
	{
		nulls = NULL;
		n = 1;
	}
	*out = (SiftSrcPrune) {.leaf = l->id,.strs = palloc(Max(n, 1) * sizeof(SiftColValue))};
	for (int i = 0; i < n; i++)
		if (nulls == NULL || !nulls[i])
		{
			const text *t = DatumGetTextPP(vals[i]);

			out->strs[out->nvals++] = (SiftColValue) {.v.s = VARDATA_ANY(t),.len = VARSIZE_ANY_EXHDR(t)};
		}
	return out->nvals > 0;
}

/* the JSON type tests of an index scan (jsonb_typeof(doc -> 'k') in the tag set of the key, and `doc ? 'k'` as every type but a missing path), and its text equalities */
SiftV1Scan
sift_v1_key_scan(Relation index, ScanKey keys, int nkeys)
{
	/* check-memory: allow the scan keys of one statement */
	SiftV1Scan	scan = {.strs = palloc(Max(nkeys, 1) * sizeof(SiftSrcPrune))};
	const SiftSrcLayout *lay = sift_v1_reader_of(false)->layout(index);

	for (int i = 0; i < nkeys; i++)
	{
		const ScanKeyData *k = &keys[i];
		const int64 arg = DatumGetInt64(k->sk_argument);
		char	   *name;

		if (k->sk_strategy != COL_STRAT_JSONB_TYPE && k->sk_strategy != COL_STRAT_JSONB_EXISTS)
		{
			scan.nstrs += str_key(index, lay, k, &scan.strs[scan.nstrs]);
			continue;
		}
		SIFT_INVARIANT(scan.ntypes < SIFT_V1_MAX_TYPE_TESTS, "an aggregate scan of index %u tests the JSON types of more than %d keys", RelationGetRelid(index), SIFT_V1_MAX_TYPE_TESTS);
		if (k->sk_strategy == COL_STRAT_JSONB_TYPE)
			scan.types[scan.ntypes++] = (SiftV1TypeTest) {.leaf = (int) (arg >> 16),.types = (uint32) (arg & 0xFFFF) >> 1};
		else
		{
			name = TextDatumGetCString(k->sk_argument);
			scan.types[scan.ntypes++] = (SiftV1TypeTest) {.leaf = v1_jsonb_ops.key_leaf(RelationGetRelid(index), false, k->sk_attno, list_make1(makeString(name))),
				.types = SIFT_SRC_JT_ALL & ~SIFT_SRC_JT_NONE,.key = true};
		}
	}
	return scan;
}

static void
zone_scan_run(const struct SiftV6Spec *spec, struct SiftV6Group *g, Relation heap, Relation index, ScanKey keys, int nkeys, Snapshot snap, const uint64 *skip, int nskip,
			  const struct SiftScopeRouter *router, struct SiftV6ScanStats *stats, struct SiftV6StarRun *star, bool (*next_chunk) (void *arg, const struct SiftTidBridge **bridge),
			  void *chunk_arg)
{
	SiftSrcFoldArgs a = {.spec = spec,.group = g,.stats = stats,.star = star,.host = index,.quals = sift_srcfold_scankey_quals(index, keys, nkeys)};
	SiftV1Scan	scan = sift_v1_key_scan(index, keys, nkeys);

	SIFT_INVARIANT(nskip == 0 && router == NULL, "Format 1 has no device parts or scope summaries to scan index %u by", RelationGetRelid(index));
	fold_run(&a, snap, &scan, next_chunk, chunk_arg);
}

static void
zone_scan_arms(struct SiftV6FuseCohort *cohort, const struct SiftV6ScanArm *arms, Relation heap, Relation index, ScanKey keys, int nkeys, Snapshot snap)
{
	MemoryContext cx = sift_mem_allocset_create(CurrentMemoryContext, "sift v1 zone arms");
	SiftV1Scan	scan = sift_v1_key_scan(index, keys, nkeys);
	V1Open		o = {.cx = cx,.snap = snap,.scan = &scan};

	sift_srcfold_arms(cohort, arms, &(SiftSrcFoldArgs) {.host = index,.quals = sift_srcfold_scankey_quals(index, keys, nkeys)}, open_source, &o);
	MemoryContextDelete(cx);
}

static bool
dict_has_dict32(Oid relid, bool is_table, int att)
{
	return false;
}

const SiftZoneOps sift_v1_zone_ops = {
	.batch_cols = SIFT_V6_MAX_COLS,
	.in_keys_max = SIFT_CPOINT_MAX_KEYS,
	.table_star = true,
	.compile_quals = zone_compile_quals,
	.bind_quals = zone_bind_quals,
	.quals_keys = zone_quals_keys,
	.scan_table = zone_scan_table,
	.scan_run = zone_scan_run,
	.scan_arms = zone_scan_arms
};

const SiftDictOps sift_v1_dict_ops = {
	.has_dict32 = dict_has_dict32
};
