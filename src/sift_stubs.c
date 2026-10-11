/*
 * sift_stubs.c - weak placeholders for entry points whose modules have not
 * landed yet. A module defining the real (strong) symbol overrides its stub
 * at link time; its owner then deletes the stub here. See the design notes MODULE
 * MAP, contract 1.
 */
#include "postgres.h"

#include "access/amapi.h"
#include "utils/selfuncs.h"

#include "sift.h"
#include "sift_am.h"
#include "sift_kernels.h"
#include "sift_sql.h"

#define WEAK __attribute__((weak))

#define NOT_IMPLEMENTED(what) \
	ereport(ERROR, \
			(errcode(ERRCODE_FEATURE_NOT_SUPPORTED), \
			 errmsg("sift: %s is not implemented yet", what)))

WEAK void
sift_kernels_init(void)
{
}

WEAK void
sift_planner_init(void)
{
}

WEAK IndexBuildResult *
sift_ambuild(Relation heap, Relation index, struct IndexInfo *indexInfo)
{
	NOT_IMPLEMENTED("index build");
	pg_unreachable();
}

WEAK void
sift_ambuildempty(Relation index)
{
	NOT_IMPLEMENTED("empty index build");
}

WEAK bool
sift_aminsert(Relation index, Datum *values, bool *isnull, ItemPointer heap_tid,
			  Relation heap, IndexUniqueCheck checkUnique, bool indexUnchanged,
			  struct IndexInfo *indexInfo)
{
	NOT_IMPLEMENTED("index insert");
	pg_unreachable();
}

WEAK IndexBulkDeleteResult *
sift_ambulkdelete(IndexVacuumInfo *info, IndexBulkDeleteResult *stats,
				  IndexBulkDeleteCallback callback, void *callback_state)
{
	NOT_IMPLEMENTED("index bulk delete");
	pg_unreachable();
}

WEAK IndexBulkDeleteResult *
sift_amvacuumcleanup(IndexVacuumInfo *info, IndexBulkDeleteResult *stats)
{
	return stats;
}

/* Prices the index out of every plan until the planner module lands */
WEAK void
sift_amcostestimate(struct PlannerInfo *root, struct IndexPath *path, double loop_count,
					Cost *indexStartupCost, Cost *indexTotalCost,
					Selectivity *indexSelectivity, double *indexCorrelation,
					double *indexPages)
{
	*indexStartupCost = 1.0e10;
	*indexTotalCost = 1.0e10;
	*indexSelectivity = 1.0;
	*indexCorrelation = 0.0;
	*indexPages = 1.0;
}

WEAK IndexScanDesc
sift_ambeginscan(Relation index, int nkeys, int norderbys)
{
	NOT_IMPLEMENTED("index scan");
	pg_unreachable();
}

WEAK void
sift_amrescan(IndexScanDesc scan, ScanKey keys, int nkeys, ScanKey orderbys, int norderbys)
{
	NOT_IMPLEMENTED("index rescan");
}

WEAK int64
sift_amgetbitmap(IndexScanDesc scan, TIDBitmap *tbm)
{
	NOT_IMPLEMENTED("bitmap index scan");
	pg_unreachable();
}

WEAK void
sift_amendscan(IndexScanDesc scan)
{
}

WEAK Datum
sift_match(PG_FUNCTION_ARGS)
{
	NOT_IMPLEMENTED("@@>");
	PG_RETURN_NULL();
}

WEAK Datum
sift_match_idx(PG_FUNCTION_ARGS)
{
	NOT_IMPLEMENTED("@@>");
	PG_RETURN_NULL();
}

/* No simplification and no index condition */
WEAK Datum
sift_match_support(PG_FUNCTION_ARGS)
{
	PG_RETURN_POINTER(NULL);
}

WEAK Datum
sift_matchsel(PG_FUNCTION_ARGS)
{
	PG_RETURN_FLOAT8(DEFAULT_MATCH_SEL);
}

WEAK Datum
sift_score(PG_FUNCTION_ARGS)
{
	NOT_IMPLEMENTED("sift.score()");
	PG_RETURN_NULL();
}

WEAK Datum
sift_max_score(PG_FUNCTION_ARGS)
{
	NOT_IMPLEMENTED("sift.max_score()");
	PG_RETURN_NULL();
}

WEAK Datum
sift_highlight(PG_FUNCTION_ARGS)
{
	NOT_IMPLEMENTED("sift.highlight()");
	PG_RETURN_NULL();
}

WEAK Datum
sift_tokenize(PG_FUNCTION_ARGS)
{
	NOT_IMPLEMENTED("sift.tokenize()");
	PG_RETURN_NULL();
}

WEAK Datum
sift_parse_query(PG_FUNCTION_ARGS)
{
	NOT_IMPLEMENTED("sift.parse_query()");
	PG_RETURN_NULL();
}

WEAK Datum
sift_promote(PG_FUNCTION_ARGS)
{
	NOT_IMPLEMENTED("sift.promote()");
	PG_RETURN_NULL();
}

WEAK Datum
sift_merge(PG_FUNCTION_ARGS)
{
	NOT_IMPLEMENTED("sift.merge()");
	PG_RETURN_NULL();
}

WEAK Datum
sift_segments(PG_FUNCTION_ARGS)
{
	NOT_IMPLEMENTED("sift.segments()");
	PG_RETURN_NULL();
}

WEAK Datum
sift_verify(PG_FUNCTION_ARGS)
{
	NOT_IMPLEMENTED("sift.verify()");
	PG_RETURN_NULL();
}
