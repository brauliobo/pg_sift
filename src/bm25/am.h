/*
 * bm25/am.h - index AM callbacks referenced by the handler. Each is defined
 * by its owning module (see DESIGN.md "Source layout").
 */
#ifndef SIFT_AM_H
#define SIFT_AM_H

#include "access/amapi.h"

/* core/handler.c: the IndexAmRoutine the handlers start from; they set the callbacks */
extern IndexAmRoutine *sift_am_routine(int strategies, int support, bool multicol);

/* bm25/build.c */
extern IndexBuildResult *sift_ambuild(Relation heap, Relation index, struct IndexInfo *indexInfo);
extern void sift_ambuildempty(Relation index);
extern char *sift_ambuildphasename(int64 phase);

/* bm25/wbuf.c */
extern bool sift_aminsert(Relation index, Datum *values, bool *isnull, ItemPointer heap_tid,
						  Relation heap, IndexUniqueCheck checkUnique, bool indexUnchanged,
						  struct IndexInfo *indexInfo);

/* bm25/maint.c */
extern IndexBulkDeleteResult *sift_ambulkdelete(IndexVacuumInfo *info, IndexBulkDeleteResult *stats,
												IndexBulkDeleteCallback callback, void *callback_state);
extern IndexBulkDeleteResult *sift_amvacuumcleanup(IndexVacuumInfo *info, IndexBulkDeleteResult *stats);

/* bm25/planner_path.c */
extern void sift_amcostestimate(struct PlannerInfo *root, struct IndexPath *path, double loop_count,
								Cost *indexStartupCost, Cost *indexTotalCost,
								Selectivity *indexSelectivity, double *indexCorrelation,
								double *indexPages);
extern void sift_planner_init(void);	/* hooks + custom scan methods, from _PG_init */

/* bm25/amscan.c */
extern IndexScanDesc sift_ambeginscan(Relation index, int nkeys, int norderbys);
extern void sift_amrescan(IndexScanDesc scan, ScanKey keys, int nkeys,
						  ScanKey orderbys, int norderbys);
extern int64 sift_amgetbitmap(IndexScanDesc scan, TIDBitmap *tbm);
extern void sift_amendscan(IndexScanDesc scan);

#endif							/* SIFT_AM_H */
