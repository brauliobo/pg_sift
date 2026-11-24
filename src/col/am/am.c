/*
 * col/am/am.c - the column access method: the handler and the routing of its callbacks to the table of the format of the relation (col/am/format_ops.h).
 * The files of a format (col/v1/) do the work; reloptions, the relcache and the caches are in the files next to this one.
 */
#include "postgres.h"
#include "utils/rel.h"
#include "access/genam.h"
#include "access/table.h"
#include "commands/extension.h"
#include "col/am.h"
#include "col/am/am_int.h"
#include "col/f2/f2.h"
#include "col/pg.h"
#include "bm25/am.h"
#include "core/meta_peek.h"
#include "core/f2/f2_progress.h"
#include "core/ops.h"
#include "core/quarantine.h"

static const SiftColFormatOps *const format_ops[] = {[1] = &sift_col_v1_ops, [2] = &sift_col_v2_ops};

const SiftColFormatOps *
sift_col_ops_of_major(uint16 major)
{
	if (major >= lengthof(format_ops) || format_ops[major] == NULL)
		ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("sift_col has no engine for format major %u in pg_sift %s", major, SIFT_MODULE_VERSION),
						errhint("Install the pg_sift release that wrote the index, or REINDEX it.")));
	return format_ops[major];
}

const SiftColFormatOps *
sift_col_ops_for_build(void)
{
	return sift_col_ops_of_major(sift_new_relation_format_major());
}

#define SCAN_OPS(scan)	(((SiftColScanHeader *) (scan)->opaque)->ops)

/* The held rows of every format: each is a test of an empty list while its format has none */
void
sift_col_flush_held(void)
{
	for (int major = 0; major < lengthof(format_ops); major++)
		if (format_ops[major] != NULL)
			format_ops[major]->flush_held();
}

/* What format major 2 does not hold stays in format major 1: a connection (the index that declares it and the indexes it connects to) and a scope that format major 2 does not summarize */
static bool
format1_only(Relation index)
{
	Relation	heap = table_open(index->rd_index->indrelid, AccessShareLock);
	bool		only = sift_col_options_connect(index)[0] != '\0' || sift_conn_children(RelationGetRelid(index)) != NIL || !sift_f2s_index_summarizable(heap, index);

	table_close(heap, NoLock);
	return only;
}

/* INCLUDE columns and UNIQUE are held by the engine of format major 2 alone: the index that the build routes to format major 1 is refused them */
static const SiftColFormatOps *
ops_for_build(Relation index)
{
	const SiftColFormatOps *ops = sift_col_ops_for_build();

	if (ops->format_major != SIFT_FORMAT2_MAJOR && (index->rd_index->indisunique || IndexRelationGetNumberOfAttributes(index) != IndexRelationGetNumberOfKeyAttributes(index)))
		ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("a sift_col index of format major 1 has no %s", index->rd_index->indisunique ? "UNIQUE" : "INCLUDE columns"),
						errhint("Leave out the connect option and the scopes the engine of format major 2 does not summarize: the index is then built in format major 2.")));
	return ops;
}

static IndexBuildResult *
route_ambuild(Relation heap, Relation index, IndexInfo *info)
{
	IndexBuildResult *result;
	bool		format1 = format1_only(index);

	if (format1)
		sift_tbl_push_new_format(SIFT_FORMAT_MAJOR);
	PG_TRY();
	{
		result = ops_for_build(index)->ambuild(heap, index, info);
	}
	PG_FINALLY();
	{
		if (format1)
			sift_tbl_pop_new_format();
	}
	PG_END_TRY();
	return result;
}

static void
route_ambuildempty(Relation index)
{
	bool		format1 = format1_only(index);

	if (format1)
		sift_tbl_push_new_format(SIFT_FORMAT_MAJOR);
	PG_TRY();
	{
		ops_for_build(index)->ambuildempty(index);
	}
	PG_FINALLY();
	{
		if (format1)
			sift_tbl_pop_new_format();
	}
	PG_END_TRY();
}

/*
 * A quarantined index is answered here, before sift_col_ops reads its block 0: a damaged metapage names no format, and the engine of no format may touch an index that left service
 * (core/quarantine.h). The write is not indexed and VACUUM goes on without the index.
 */
static bool
route_aminsert(Relation index, Datum *values, bool *isnull, ItemPointer heap_tid, Relation heap, IndexUniqueCheck checkUnique, bool indexUnchanged,
			   IndexInfo *indexInfo)
{
	if (sift_quarantine_skips(index, SIFT_SKIP_WRITE))
		return false;
	return sift_col_ops(index)->aminsert(index, values, isnull, heap_tid, heap, checkUnique, indexUnchanged, indexInfo);
}

static IndexBulkDeleteResult *
vacuum_stats(IndexBulkDeleteResult *stats)
{
	return stats != NULL ? stats : palloc0(sizeof(IndexBulkDeleteResult));
}

static IndexBulkDeleteResult *
route_ambulkdelete(IndexVacuumInfo *info, IndexBulkDeleteResult *stats, IndexBulkDeleteCallback callback, void *callback_state)
{
	if (sift_quarantine_skips(info->index, SIFT_SKIP_VACUUM))
		return vacuum_stats(stats);
	return sift_col_ops(info->index)->ambulkdelete(info, stats, callback, callback_state);
}

static IndexBulkDeleteResult *
route_amvacuumcleanup(IndexVacuumInfo *info, IndexBulkDeleteResult *stats)
{
	if (sift_quarantine_skips(info->index, SIFT_SKIP_VACUUM))
		return vacuum_stats(stats);
	return sift_col_ops(info->index)->amvacuumcleanup(info, stats);
}

static void
route_amcostestimate(PlannerInfo *root, IndexPath *path, double loop_count, Cost *startup, Cost *total, Selectivity *selectivity, double *correlation,
					 double *pages)
{
	Relation	index = index_open(path->indexinfo->indexoid, AccessShareLock);
	const SiftColFormatOps *ops;

	if (sift_quarantine_cost(index, path->indexinfo->pages, startup, total, selectivity, correlation, pages))
	{
		index_close(index, NoLock);
		return;
	}
	ops = sift_col_ops(index);
	index_close(index, NoLock);
	ops->amcostestimate(root, path, loop_count, startup, total, selectivity, correlation, pages);
}

/* The session that reads pg_stat_progress_create_index is not the one that builds: the number of the subphase alone says the format that names it (core/f2/f2_progress.h) */
static char *
route_ambuildphasename(int64 phase)
{
	return (phase >= SIFT_F2_PHASE_SAMPLE ? &sift_col_v2_ops : &sift_col_v1_ops)->ambuildphasename(phase);
}

/* A plan made before the index left service is refused as a scan that reads its directory refuses it: with 40001, and the plans that name the index are dropped */
static IndexScanDesc
route_ambeginscan(Relation index, int nkeys, int norderbys)
{
	if (sift_index_quarantined(index))
		sift_quarantine_net(index, SIFT_META_QUARANTINED);
	return sift_col_ops(index)->ambeginscan(index, nkeys, norderbys);
}

static void
route_amrescan(IndexScanDesc scan, ScanKey keys, int nkeys, ScanKey orderbys, int norderbys)
{
	SCAN_OPS(scan)->amrescan(scan, keys, nkeys, orderbys, norderbys);
}

static int64
route_amgetbitmap(IndexScanDesc scan, TIDBitmap *tbm)
{
	return SCAN_OPS(scan)->amgetbitmap(scan, tbm);
}

static bool
route_amgettuple(IndexScanDesc scan, ScanDirection dir)
{
	return SCAN_OPS(scan)->amgettuple(scan, dir);
}

static void
route_amendscan(IndexScanDesc scan)
{
	SCAN_OPS(scan)->amendscan(scan);
}

static void
route_ammarkpos(IndexScanDesc scan)
{
	SCAN_OPS(scan)->ammarkpos(scan);
}

static void
route_amrestrpos(IndexScanDesc scan)
{
	SCAN_OPS(scan)->amrestrpos(scan);
}

static bool
route_amcanreturn(Relation index, int attno)
{
	if (sift_index_quarantined(index))
		return false;
	return sift_col_ops(index)->amcanreturn(index, attno);
}

static bool
col_amvalidate(Oid opclassoid)
{
	return true;
}

#if PG_VERSION_NUM >= 180000
/* The strategies of the operator classes are btree's (col/am/preds.h): the planner and the executor name a comparison by its number */
static CompareType
col_translatestrategy(StrategyNumber strategy, Oid opfamily)
{
	return strategy >= BTLessStrategyNumber && strategy <= BTGreaterStrategyNumber ? (CompareType) strategy : COMPARE_INVALID;
}

static StrategyNumber
col_translatecmptype(CompareType cmptype, Oid opfamily)
{
	return cmptype >= COMPARE_LT && cmptype <= COMPARE_GT ? (StrategyNumber) cmptype : InvalidStrategy;
}
#endif

Datum
sift_col_handler(PG_FUNCTION_ARGS)
{
	IndexAmRoutine *am = sift_am_routine(COL_STRAT_ARRAY_OVERLAP, 1, true);

	am->ambuild = route_ambuild;
	am->ambuildempty = route_ambuildempty;
	am->aminsert = route_aminsert;
	am->ambulkdelete = route_ambulkdelete;
	am->amvacuumcleanup = route_amvacuumcleanup;
	am->amcostestimate = route_amcostestimate;
	am->amoptions = am_col_amoptions;
	am->ambuildphasename = route_ambuildphasename;
	am->amvalidate = col_amvalidate;
	am->ambeginscan = route_ambeginscan;
	am->amrescan = route_amrescan;
	am->amgetbitmap = route_amgetbitmap;
	am->amgettuple = route_amgettuple;
	am->ammarkpos = route_ammarkpos;
	am->amrestrpos = route_amrestrpos;
	am->amcanreturn = route_amcanreturn;
	am->amendscan = route_amendscan;
	am->amsearcharray = am->amsearchnulls = true;
	am->amcaninclude = true;
	am->amcanunique = true;
	/*
	 * No flag of backward scans: an index of format major 1 cannot turn its scan round, and a cursor that scrolls reads over a Materialize instead (the ordered scans of format major 2 go backward on their own).
	 * Only an index of format major 2 whose key_order is the order of its columns has an order to give: the planner is told which (sift_f2_ord_plan_init). An access method that orders wants a
	 * comparison function with two arguments in the first member of every class, and the default class of the released scripts (which a fresh install runs again) has a placeholder with one:
	 * the scripts that create classes run before the order is offered.
	 */
	am->amcanorder = !creating_extension;
#if PG_VERSION_NUM >= 180000
	am->amconsistentordering = !creating_extension;
	am->amtranslatestrategy = col_translatestrategy;
	am->amtranslatecmptype = col_translatecmptype;
#endif

	PG_RETURN_POINTER(am);
}
