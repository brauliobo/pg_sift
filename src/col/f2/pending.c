/*
 * col/f2/pending.c - the addresses a pending delete of a partition has dead for a snapshot.
 */
#include "postgres.h"

#include "col/f2/f2.h"
#include "col/f2/del/pend.h"
#include "col/f2/part/vis.h"
#include "core/check.h"
#include "core/xid_cache.h"

static int
addr_cmp(const void *a, const void *b)
{
	uint64		x = *(const uint64 *) a,
				y = *(const uint64 *) b;

	return (x > y) - (x < y);
}

/* The TIDs the snapshot sees deleted by a pending delete of the partition, in the order of the chain */
static uint64 *
unsorted_pending_dead(Relation rel, const SiftPartitionEntry *e, Snapshot snap, int *n)
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
		if ((recs[i].tid & SIFT_PD_TID_SUCC) == 0 && sift_vis_deleted_cached(&xc, recs[i].xmax, recs[i].cmax))
		{
			SIFT_INVARIANT(m < nrecs, "a pending deletion is offered once");
			dead[m++] = recs[i].tid & SIFT_PD_TID_MASK;
		}
	*n = m;
	return dead;
}

/* The TIDs the snapshot sees deleted by a pending delete of the partition, sorted by address */
uint64 *
sift_f2_pending_dead(Relation rel, const SiftPartitionEntry *e, Snapshot snap, int *n)
{
	uint64	   *dead = unsorted_pending_dead(rel, e, snap, n);

	if (dead != NULL)
		qsort(dead, *n, sizeof(uint64), addr_cmp);
	return dead;
}

/* The staged deletes of the transaction as the set every partition of a scan adds its pending deletes to: built once, NULL for none */
SiftPendDelIndex *
sift_f2_staged_index(const uint64 *staged, int nstaged)
{
	return sift_pdel_idx_chain(staged, nstaged, NULL);
}

/* The set of the pending deletes of the partition the snapshot sees, with the staged ones (staged_px): the one of the transaction itself when the partition has none; NULL when neither has */
SiftPendDelIndex *
sift_f2_pending_index(Relation rel, const SiftPartitionEntry *e, Snapshot snap, const SiftPendDelIndex *staged_px)
{
	int			n;
	uint64	   *dead = unsorted_pending_dead(rel, e, snap, &n);
	SiftPendDelIndex *px = sift_pdel_idx_chain(dead, n, staged_px);

	if (dead != NULL)
		pfree(dead);
	return px;
}
