/*
 * col/f2/slab_view.h - the micro-slabs of a partition as parts in memory, for the views of a scan.
 *
 * A serial scan writes nothing and takes no lock of a partition (sift_f2_view), on a primary as on a standby: its view lists the slab of a partition as one more part of the partition,
 * made in memory from the rows of the slab as they are when a pin opens it; the descriptor says so (SIFT_PART_EXT_SLAB) and names the head of the slab and its rows. A process that did not
 * list the view reads the slab again, which the writers of a directory of shards reorder: the leader of a parallel scan on a primary writes the slabs out instead (sift_f2_parallel_view).
 */
#ifndef SIFT_F2_SLAB_VIEW_H
#define SIFT_F2_SLAB_VIEW_H

#include "col/f2/cache.h"
#include "col/f2/store/store.h"

static inline bool
sift_f2_slab_desc(const SiftPartDesc *d)
{
	return (d->ext_flags & SIFT_PART_EXT_SLAB) != 0;
}

/* snap with the slab of each partition that has one added to the parts of its list (snap itself when none has): valid until mcxt is reset */
extern const SiftPlanSnapshot *sift_f2_slab_view(Relation rel, SiftF2Store *store, const SiftPlanSnapshot *snap, MemoryContext mcxt);

/*
 * The view a parallel scan lists its units from, in mcxt: on a primary the slabs are written out as parts first and the view lists none, so that every participant opens the parts the leader
 * listed. A slab that writers start after the flush holds no row the snapshot of the scan sees: it was taken before, and a transaction appends its rows before it commits.
 */
extern const SiftPlanSnapshot *sift_f2_parallel_view(Relation rel, SiftF2Store *store, MemoryContext mcxt);

/* The part of a slab descriptor, made in CurrentMemoryContext: the rows the slab holds that the snapshot of the statement sees, the first d->nrows of them */
extern SiftPart *sift_f2_slab_part(Relation rel, const SiftF2Store *store, const SiftPartDesc *d, uint32 part_id);

#endif							/* SIFT_F2_SLAB_VIEW_H */
