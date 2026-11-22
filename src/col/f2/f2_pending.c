/*
 * col/f2/f2_pending.c - the addresses a pending delete of a partition has dead for a snapshot.
 */
#include "postgres.h"

#include "col/f2/f2.h"
#include "col/f2/pend_del.h"
#include "col/f2/part_vis.h"
#include "core/check.h"
#include "core/xid_cache.h"

static int
addr_cmp(const void *a, const void *b)
{
	uint64		x = *(const uint64 *) a,
				y = *(const uint64 *) b;

	return (x > y) - (x < y);
}

/* The TIDs the snapshot sees deleted by a pending delete of the partition, sorted by address */
uint64 *
sift_f2_pending_dead(Relation rel, const SiftPartitionEntry *e, Snapshot snap, int *n)
{
	int			nrecs = 0,
				m = 0;
	SiftPendDelRec *recs;
	uint64	   *dead;
	SiftXidCache xc;

	*n = 0;
	if (e->pend_del_blk == InvalidBlockNumber)
		return NULL;
	recs = sift_pend_del_read(rel, e->part_id, e->pend_del_blk, &nrecs, NULL, NULL);
	dead = palloc(Max(nrecs, 1) * sizeof(uint64));
	sift_xid_cache_init(&xc, snap);
	for (int i = 0; i < nrecs; i++)
		if (sift_vis_deleted_cached(&xc, recs[i].xmax, recs[i].cmax))
		{
			SIFT_INVARIANT(m < nrecs, "a pending deletion is offered once");
			dead[m++] = recs[i].tid;
		}
	qsort(dead, m, sizeof(uint64), addr_cmp);
	*n = m;
	return dead;
}
