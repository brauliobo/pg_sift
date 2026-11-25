/*
 * col/f2/cube/mask.h - the granules of a summary that a scan for some of its keys has to read: a bit for each granule that may hold a row of a member of the keys. Portable.
 */
#ifndef SIFT_COL_SCOPE_V6_RMASK_H
#define SIFT_COL_SCOPE_V6_RMASK_H

#include "core/host.h"

#include "col/f2/cube/meta.h"

typedef struct SiftScopeReadMask
{
	uint32		n;				/* the granules of the summary; a granule at n or past it is one the summary does not know: it is read */
	uint64	   *want;
	uint64		kept;			/* granules with want set */
} SiftScopeReadMask;

static inline bool
sift_rmask_want(const SiftScopeReadMask *k, uint32 g)
{
	return g >= k->n || ((k->want[g >> 6] >> (g & 63)) & 1) != 0;
}

/*
 * The granules of keys (ascending, distinct) and, for the members whose pairs changed since the summary was written (changed, ascending; NULL when none), every granule whose member zone holds one
 * of them: src gives the zones. A key the summary lacks names no granule.
 */
extern void sift_scope_rmask_build(const SiftScopeMeta *m, const int64 *keys, uint32 nkeys, const int64 *changed, uint32 nchanged, const SiftScopeGranSource *src, SiftScopeReadMask *out);

#endif							/* SIFT_COL_SCOPE_V6_RMASK_H */
