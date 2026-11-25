/*
 * col/f2/slab_view.c - the micro-slabs of a partition as parts in memory, for a standby (f2_slab_view.h).
 *
 * check-memory: allow-allocs the rows of one slab (at most SIFT_L0_SLAB_FLUSH_ROWS) and the partitions of one manifest
 */
#include "postgres.h"

#include "utils/snapmgr.h"

#include "col/f2/slab_view.h"
#include "col/f2/l0/slab.h"
#include "col/f2/rowbuf/rowbuf.h"
#include "core/memtrack.h"

static Snapshot
statement_snapshot(void)
{
	return ActiveSnapshotSet() ? GetActiveSnapshot() : GetTransactionSnapshot();
}

/* The first limit rows of rb, in the order they were added: what a slab held when the leader of a scan listed it */
static SiftRowBuf *
first_rows(SiftRowBuf *rb, const SiftPartSpec *spec, int limit)
{
/* check-memory: allow bounded to one micro-slab: SIFT_L0_SLAB_MAX_PAGES pages */
	SiftRowBuf *kept = sift_rowbuf_new(spec);

	for (int r = 0; r < limit; r++)
		sift_rowbuf_copy_row(kept, rb, r, rb->xmin[r], rb->cmin[r], rb->subxid[r]);
	sift_rowbuf_free(rb);
	return kept;
}

/* The part of the rows of the slab at head, or NULL when it has none the statement sees; limit < 0: all of them */
static SiftPart *
slab_part(Relation rel, const SiftPartSpec *spec, uint64 part_id, BlockNumber head, int limit, SiftPartDesc *d)
{
/* check-memory: allow bounded to one micro-slab: SIFT_L0_SLAB_MAX_PAGES pages */
	SiftRowBuf *rb = sift_rowbuf_new(spec);
	SiftColIn  *cols;
	SiftRows	r;

	sift_l0_slab_read(rel, part_id, head, rb, spec, statement_snapshot(), NULL, NULL);
	if (limit >= 0 && rb->n < limit)
		ereport(ERROR, (errcode(ERRCODE_T_R_SERIALIZATION_FAILURE), errmsg("the micro-slab of partition %llu of \"%s\" changed while the statement read it", (unsigned long long) part_id,
																		   RelationGetRelationName(rel)),
						errhint("Run the statement again.")));
	if (limit >= 0 && rb->n > limit)
		rb = first_rows(rb, spec, limit);
	if (rb->n == 0)
		return NULL;
	cols = palloc(Max(spec->ncols, 1) * sizeof(SiftColIn));
	sift_rowbuf_sort(rb, spec);
	sift_rowbuf_cluster(rb, spec, SIFT_TIER_HOT_L0);
	r = sift_rowbuf_rows(rb, cols, spec);
	return sift_part_rows_in_memory(rel, (uint32) part_id, SIFT_TIER_HOT_L0, &r, d);
}

SiftPart *
sift_f2_slab_part(Relation rel, const SiftF2Store *store, const SiftPartDesc *d, uint32 part_id)
{
	SiftPartDesc made;
	SiftPart   *part = slab_part(rel, &store->spec, part_id, d->header_blk, (int) d->nrows, &made);

	if (part == NULL)
		ereport(ERROR, (errcode(ERRCODE_T_R_SERIALIZATION_FAILURE), errmsg("the micro-slab of partition %u of \"%s\" is gone", part_id, RelationGetRelationName(rel)),
						errhint("Run the statement again.")));
	return part;
}

/* The descriptor the part of the slab of e has, or false when the statement sees no row in it */
static bool
slab_desc(Relation rel, const SiftF2Store *store, const SiftPartitionEntry *e, SiftPartDesc *d)
{
	MemoryContext scratch = sift_mem_allocset_create(CurrentMemoryContext, "sift f2 slab view");
	MemoryContext caller = MemoryContextSwitchTo(scratch);
	bool		found = slab_part(rel, &store->spec, e->part_id, e->l0_slab_blk, -1, d) != NULL;

	MemoryContextSwitchTo(caller);
	MemoryContextDelete(scratch);
	if (found)
	{
		d->header_blk = e->l0_slab_blk;
		d->ext_flags |= SIFT_PART_EXT_SLAB;
	}
	return found;
}

const SiftPlanSnapshot *
sift_f2_slab_view(Relation rel, SiftF2Store *store, const SiftPlanSnapshot *snap, MemoryContext mcxt)
{
	SiftPlanSnapshot *view = NULL;

	for (int i = 0; i < snap->npartitions; i++)
	{
		SiftPartDesc d;
		SiftPlanPartList *list;
		SiftPartDesc *parts;

		if (snap->partitions[i].l0_slab_blk == InvalidBlockNumber || !slab_desc(rel, store, &snap->partitions[i], &d))
			continue;
		if (view == NULL)
		{
			view = MemoryContextAlloc(mcxt, sizeof(SiftPlanSnapshot));
			*view = *snap;
			view->partlists = MemoryContextAlloc(mcxt, Max(snap->npartitions, 1) * sizeof(SiftPlanPartList));
			memcpy(view->partlists, snap->partlists, snap->npartitions * sizeof(SiftPlanPartList));
			view->slabs_in_memory = true;
		}
		list = &view->partlists[i];
		parts = MemoryContextAlloc(mcxt, (list->nparts + 1) * sizeof(SiftPartDesc));
		memcpy(parts, list->parts, list->nparts * sizeof(SiftPartDesc));
		parts[list->nparts] = d;
		list->parts = parts;
		list->nparts++;
	}
	return view != NULL ? view : snap;
}
