/*
 * col/f2/f2_slab_view.h - the micro-slabs of a partition as parts in memory, for a standby.
 *
 * A primary writes a micro-slab out as a part before a scan reads it (sift_f2_view). A standby writes nothing, so its view lists the slab of a partition as one more part of the partition,
 * made in memory from the rows of the slab as they are when a pin opens it; the descriptor says so (SIFT_PART_EXT_SLAB) and names the head of the slab and its rows. A part made twice from the
 * same slab is the same part, which is what lets a worker of a parallel scan open the part the leader listed.
 */
#ifndef SIFT_F2_SLAB_VIEW_H
#define SIFT_F2_SLAB_VIEW_H

#include "col/f2/f2_cache.h"
#include "col/f2/f2_store.h"

static inline bool
sift_f2_slab_desc(const SiftPartDesc *d)
{
	return (d->ext_flags & SIFT_PART_EXT_SLAB) != 0;
}

/* snap with the slab of each partition that has one added to the parts of its list (snap itself when none has): valid until mcxt is reset */
extern const SiftPlanSnapshot *sift_f2_slab_view(Relation rel, SiftF2Store *store, const SiftPlanSnapshot *snap, MemoryContext mcxt);

/* The part of a slab descriptor, made in CurrentMemoryContext: the rows the slab holds that the snapshot of the statement sees, the first d->nrows of them */
extern SiftPart *sift_f2_slab_part(Relation rel, const SiftF2Store *store, const SiftPartDesc *d, uint32 part_id);

#endif							/* SIFT_F2_SLAB_VIEW_H */
