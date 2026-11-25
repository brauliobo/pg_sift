/*
 * col/f2/f2_store_ops_zone.c - SiftZoneOps and SiftStoreParOps of format major 2: the aggregate scan of the parts of a relation (f2_v6_scan.c) over the granules its zones, dictionaries and skipping
 * indexes leave, the predicates of the statement's quals a sift_table decides on its leaves (f2_store_scan.h), and the units of a parallel scan.
 */
#include "postgres.h"

#include "col/f2/f2_store.h"
#include "col/f2/f2_store_ops_int.h"
#include "col/f2/f2_store_scan.h"
#include "col/f2/f2_v6_scan.h"

static struct SiftStoreQuals *
zone_compile_quals(Relation heap, List *indexqual, MemoryContext cx)
{
	SiftStoreQuals *q = MemoryContextAllocZero(cx, sizeof(SiftStoreQuals));

	sift_f2_store_compile_quals(heap, sift_f2_store_open(heap), indexqual, cx, q);
	return q;
}

static void
zone_bind_quals(Relation heap, struct SiftStoreQuals *q, ExprContext *econtext)
{
	sift_f2_store_bind_quals(heap, sift_f2_store_open(heap), q, econtext);
}

static bool
zone_quals_keys(const struct SiftStoreQuals *q, ScanKey *keys, int *nkeys)
{
	if (!q->possible)
		return false;
	*keys = q->keys;
	*nkeys = q->nkeys;
	return true;
}

const SiftZoneOps sift_f2_zone_ops = {
	.table_text = true,
	.table_star = true,
	.compile_quals = zone_compile_quals,
	.bind_quals = zone_bind_quals,
	.quals_keys = zone_quals_keys,
	.scan_table = sift_v6_scan_table,
	.scan_run = sift_v6_scan_run_ext,
	.scan_arms = sift_v6_scan_run_arms
};

const SiftStoreParOps sift_f2_par_ops = {
	.begin = sift_v6_scan_begin,
	.plan = sift_v6_scan_plan,
	.morsel_begin = sift_v6_scan_morsel_begin,
	.morsel = sift_v6_scan_morsel,
	.end = sift_v6_scan_end
};
