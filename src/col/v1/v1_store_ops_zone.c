/*
 * col/v1/v1_store_ops_zone.c - SiftZoneOps and SiftDictOps of Format 1: the aggregate scan of a sift_col index or a sift_table of Format 1 (col/source_fold.h folds the batches of its source into the
 * groups of SiftAggV6), and the predicates of the statement's quals a sift_table decides. The source skips the units whose zone of an integer column lies outside the bounds of the quals, vets the rows
 * against the tombstones and the snapshot, and decides the quals on the rows of the batches; the write buffers are units like the others. A text leaf is given as SVEC_STRVIEW: Format 1 keeps no
 * dictionary a batch aggregates by its codes, so it has none to offer (has_dict32 is false).
 */
#include "postgres.h"

#include "executor/executor.h"
#include "utils/memutils.h"

#include "col/am.h"
#include "col/source_fold.h"
#include "col/v1/v1_store_ops.h"
#include "core/memtrack.h"
#include "core/store_ops.h"

typedef struct V1Open
{
	MemoryContext cx;
	Snapshot	snap;
	const SiftV1Scan *scan;
} V1Open;

static SiftV6Source *
open_source(void *arg, const SiftV6Input *in, const SiftV6Vexpr *quals)
{
	const V1Open *o = arg;

	return sift_v1_source_open(o->cx, o->snap, in, quals, o->scan);
}

static struct SiftStoreQuals *
zone_compile_quals(Relation heap, List *indexqual, MemoryContext cx)
{
	MemoryContext old = MemoryContextSwitchTo(cx);
	struct SiftStoreQuals *q = palloc0(sizeof(struct SiftStoreQuals));

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

static void
fold_run(const SiftSrcFoldArgs *a, Snapshot snap, const SiftV1Scan *scan)
{
	MemoryContext cx = sift_mem_allocset_create(CurrentMemoryContext, "sift v1 zone scan");
	V1Open		o = {.cx = cx,.snap = snap,.scan = scan};
	SiftSrcFold *f = sift_srcfold_new(a, sift_mem_allocset_create(cx, "sift v1 zone fold"));

	sift_srcfold_run(f, open_source, &o);
	MemoryContextDelete(cx);
}

static void
zone_scan_table(const struct SiftV6Spec *spec, struct SiftV6Group *g, Relation rel, const struct SiftStoreQuals *quals, Snapshot snap, const uint64 *skip, int nskip,
				struct SiftV6ScanStats *stats)
{
	SIFT_INVARIANT(nskip == 0, "Format 1 has no device parts to leave out of the scan of table %u", RelationGetRelid(rel));
	fold_run(&(SiftSrcFoldArgs) {.spec = spec,.group = g,.stats = stats,.host = rel,.quals = quals->clauses,.estate = quals->estate}, snap, NULL);
}

/* the JSON type tests of an index scan: jsonb_typeof(doc -> 'k') in the tag set of the key, and `doc ? 'k'` as every type but a missing path */
static SiftV1Scan
key_scan(Relation index, ScanKey keys, int nkeys)
{
	SiftV1Scan	scan = {0};

	for (int i = 0; i < nkeys; i++)
	{
		const ScanKeyData *k = &keys[i];
		const int64 arg = DatumGetInt64(k->sk_argument);
		char	   *name;

		if (k->sk_strategy != COL_STRAT_JSONB_TYPE && k->sk_strategy != COL_STRAT_JSONB_EXISTS)
			continue;
		SIFT_INVARIANT(scan.ntypes < SIFT_V1_MAX_TYPE_TESTS, "an aggregate scan of index %u tests the JSON types of more than %d keys", RelationGetRelid(index), SIFT_V1_MAX_TYPE_TESTS);
		if (k->sk_strategy == COL_STRAT_JSONB_TYPE)
			scan.types[scan.ntypes++] = (SiftV1TypeTest) {.leaf = (int) (arg >> 16),.types = (uint32) (arg & 0xFFFF) >> 1};
		else
		{
			name = TextDatumGetCString(k->sk_argument);
			scan.types[scan.ntypes++] = (SiftV1TypeTest) {.leaf = v1_jsonb_ops.key_leaf(RelationGetRelid(index), k->sk_attno, name, strlen(name)),
				.types = SIFT_SRC_JT_ALL & ~SIFT_SRC_JT_NONE};
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
	SiftV1Scan	scan = key_scan(index, keys, nkeys);
	const struct SiftTidBridge *bridge = NULL;

	SIFT_INVARIANT(nskip == 0 && router == NULL, "Format 1 has no device parts or scope summaries to scan index %u by", RelationGetRelid(index));
	if (next_chunk == NULL)
		fold_run(&a, snap, &scan);
	while (next_chunk != NULL && next_chunk(chunk_arg, &bridge))
	{
		sift_srcfold_count_chunk(stats, bridge);
		scan.bridge = bridge;
		fold_run(&a, snap, &scan);
	}
}

static void
zone_scan_arms(struct SiftV6FuseCohort *cohort, const struct SiftV6ScanArm *arms, Relation heap, Relation index, ScanKey keys, int nkeys, Snapshot snap)
{
	MemoryContext cx = sift_mem_allocset_create(CurrentMemoryContext, "sift v1 zone arms");
	SiftV1Scan	scan = key_scan(index, keys, nkeys);
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
