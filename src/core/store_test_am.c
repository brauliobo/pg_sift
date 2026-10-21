/*
 * core/store_test_am.c - the sift_store_test index access method of the sift._test_* functions (core/store_test.c): ambuild writes one test
 * segment per heap row; the index cannot be scanned. Compiled only with SIFT_TESTING.
 */
#include "postgres.h"

#ifdef SIFT_TESTING

#include "access/amapi.h"
#include "funcapi.h"

#include "core/store.h"

PG_FUNCTION_INFO_V1(sift_test_am_handler);

typedef struct BuildState
{
	int			n;
	int			max;
	int32	   *v;
} BuildState;

static void
build_cb(Relation index, ItemPointer tid, Datum *values, bool *isnull, bool alive, void *arg)
{
	BuildState *bs = arg;

	if (isnull[0])
		return;
	if (bs->n == bs->max)
	{
		bs->max = Max(16, bs->max * 2);
		bs->v = bs->v ? repalloc(bs->v, bs->max * sizeof(int32)) : palloc(bs->max * sizeof(int32));
	}
	bs->v[bs->n++] = DatumGetInt32(values[0]);
}

typedef struct Publish
{
	int			n;
	SiftSegEntry *entries;
} Publish;

static void
publish_all(SiftMetaPageData *meta, SiftDirectoryData *dir, Page extra, void *arg)
{
	Publish    *p = arg;

	meta->next_gen = p->n + 1;
	memcpy(dir->segs, p->entries, p->n * sizeof(SiftSegEntry));
	dir->nsegs = p->n;
}

static IndexBuildResult *
test_ambuild(Relation heap, Relation index, IndexInfo *info)
{
	BuildState	bs = {0};
	SiftWriteTarget *wt = sift_wt_begin(index, SIFT_WRITE_BULK);
	Publish		pub;
	IndexBuildResult *res = palloc0(sizeof(IndexBuildResult));

	res->heap_tuples = table_index_build_scan(heap, index, info, true, false, build_cb, &bs, NULL);
	pub.n = Min(bs.n, SIFT_MAX_SEGMENTS);
	pub.entries = palloc(Max(pub.n, 1) * sizeof(SiftSegEntry));
	for (int i = 0; i < pub.n; i++)
		sift_test_write_segment(wt, i + 1, i64abs((int64) bs.v[i]), bs.v[i] < 0 ? 0 : (uint64) bs.v[i] + 1, 0, 0, &pub.entries[i]);
	sift_store_init_meta(wt, sift_index_options(index), 0);
	sift_wt_finish(wt);
	if (pub.n > 0)
		sift_dir_update(index, &(SiftDirChange) {.fn = publish_all, .arg = &pub, .extra = InvalidBuffer});
	res->index_tuples = pub.n;
	return res;
}

static void
test_ambuildempty(Relation index)
{
	SiftWriteTarget *wt = sift_wt_begin(index, SIFT_WRITE_INIT);

	sift_store_init_meta(wt, sift_index_options(index), 0);
	sift_wt_finish(wt);
}

static bool
test_aminsert(Relation index, Datum *values, bool *isnull, ItemPointer tid, Relation heap,
			  IndexUniqueCheck check, bool unchanged, IndexInfo *info)
{
	return false;
}

static IndexBulkDeleteResult *
test_ambulkdelete(IndexVacuumInfo *info, IndexBulkDeleteResult *stats,
				  IndexBulkDeleteCallback cb, void *state)
{
	return stats ? stats : palloc0(sizeof(IndexBulkDeleteResult));
}

static IndexBulkDeleteResult *
test_amvacuumcleanup(IndexVacuumInfo *info, IndexBulkDeleteResult *stats)
{
	return stats;
}

static void
test_amcostestimate(struct PlannerInfo *root, struct IndexPath *path, double loop_count, Cost *startup,
					Cost *total, Selectivity *sel, double *corr, double *pages)
{
	*startup = *total = 1.0e10;
	*sel = 1.0;
	*corr = 0.0;
	*pages = 1.0;
}

static bytea *
test_amoptions(Datum reloptions, bool validate)
{
	return NULL;
}

static bool
test_amvalidate(Oid opclassoid)
{
	return true;
}

static IndexScanDesc
test_ambeginscan(Relation index, int nkeys, int norderbys)
{
	elog(ERROR, "sift_store_test indexes cannot be scanned");
	pg_unreachable();
}

static void
test_amrescan(IndexScanDesc scan, ScanKey keys, int nkeys, ScanKey orderbys, int norderbys)
{
}

static void
test_amendscan(IndexScanDesc scan)
{
}

Datum
sift_test_am_handler(PG_FUNCTION_ARGS)
{
	IndexAmRoutine *am = makeNode(IndexAmRoutine);

	am->amstrategies = 1;
	am->amkeytype = InvalidOid;
	am->ambuild = test_ambuild;
	am->ambuildempty = test_ambuildempty;
	am->aminsert = test_aminsert;
	am->ambulkdelete = test_ambulkdelete;
	am->amvacuumcleanup = test_amvacuumcleanup;
	am->amcostestimate = test_amcostestimate;
	am->amoptions = test_amoptions;
	am->amvalidate = test_amvalidate;
	am->ambeginscan = test_ambeginscan;
	am->amrescan = test_amrescan;
	am->amendscan = test_amendscan;
	PG_RETURN_POINTER(am);
}

#endif							/* SIFT_TESTING */
