/*
 * col/f2/scope/route.h - a summary as the router of a scan: the granules of each part that may hold a row of a queried key (the scan reads no other). For a statement the cubes cannot answer
 * (a filter on leaves the cubes do not hold, a join the summary does not serve) that still reads the facts of one key.
 */
#ifndef SIFT_F2_SCOPE_ROUTE_H
#define SIFT_F2_SCOPE_ROUTE_H

#include "access/relscan.h"

#include "col/scope/scope.h"
#include "col/scope_v6/mask.h"

typedef struct SiftScopeRoutePart
{
	uint64		part_gen;
	uint32		base;			/* the global number of its granule 0 in the mask */
	uint32		ngranules;
} SiftScopeRoutePart;

typedef struct SiftScopeRouter
{
	SiftScopeReadMask mask;
	uint32		nparts;
	SiftScopeRoutePart *parts;	/* ascending by part_gen; a part the summary does not name (written since) is read whole, and so is every slab */
} SiftScopeRouter;

/*
 * The router of the keys (ascending, distinct) of scope ord of rel (a sift_col index or a sift_table) as the snapshot sees it, in mcxt; NULL when the scope has no summary the statement may read, the
 * journal cannot say which members changed, or the keys take more than 60% of the granules (a router prunes little then): the scan reads every granule.
 */
extern SiftScopeRouter *sift_f2_scope_build_router(Relation rel, int ord, Snapshot snap, const int64 *keys, uint32 nkeys, SiftKeyMap *km, MemoryContext mcxt);

extern bool sift_scope_router_wants(const SiftScopeRouter *r, uint64 part_gen, uint32 granule);

/* The scan reads only the granules of the router (it must outlive the scan; NULL: every one); call it after the rescan */
extern void sift_f2_scan_set_scope_router(IndexScanDesc scan, const SiftScopeRouter *router);

#endif							/* SIFT_F2_SCOPE_ROUTE_H */
