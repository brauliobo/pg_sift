/*
 * col/f2/slab_view.c - the micro-slabs of a partition as parts in memory, for a standby (f2_slab_view.h).
 *
 * check-memory: allow-allocs the rows of one slab (at most SIFT_L0_SLAB_FLUSH_ROWS) and the partitions of one manifest
 */
#include "postgres.h"

#include "access/xlog.h"
#include "lib/ilist.h"
#include "utils/snapmgr.h"

#include "col/f2/slab_view.h"
#include "col/f2/gbuf/gbuf.h"
#include "col/f2/l0/slab.h"
#include "col/f2/rowbuf/rowbuf.h"
#include "core/f2/gate.h"
#include "core/memtrack.h"

#define SLAB_REREAD_GATE	21	/* testing: a process reads again a slab of a view that it did not keep the rows of (the gates of core/f2/gate.h are 2 to 17, col/f2/uniq/shm.c has 20) */

/*
 * The rows a view read of each slab, kept while the view is: a part this backend makes again from the descriptor is made from them. A directory of hot shards is read shard by shard and
 * newest page first, so a second read of a slab its writers append to between the two reads lists its rows in another order, and its first nrows rows are not the rows the view saw
 */
typedef struct SlabRows
{
	dlist_node	node;
	RelFileLocator locator;
	uint64		part_id;
	BlockNumber head;
	SiftRowBuf *rb;
} SlabRows;

static dlist_head views_rows = DLIST_STATIC_INIT(views_rows);

static void
forget_rows(void *arg)
{
	dlist_delete(&((SlabRows *) arg)->node);
}

static void
keep_rows(Relation rel, uint64 part_id, BlockNumber head, SiftRowBuf *rb, MemoryContext mcxt)
{
	SlabRows   *k = MemoryContextAlloc(mcxt, sizeof(SlabRows));
	MemoryContextCallback *cb = MemoryContextAlloc(mcxt, sizeof(MemoryContextCallback));

	*k = (SlabRows) {.locator = rel->rd_locator,.part_id = part_id,.head = head,.rb = rb};
	*cb = (MemoryContextCallback) {forget_rows, k, NULL};
	MemoryContextRegisterResetCallback(mcxt, cb);
	dlist_push_head(&views_rows, &k->node);
}

/* The rows a view of this backend kept of the slab of the descriptor: the newest view first */
static const SiftRowBuf *
kept_rows(Relation rel, const SiftPartDesc *d, uint64 part_id)
{
	dlist_iter	it;

	dlist_foreach(it, &views_rows)
	{
		SlabRows   *k = dlist_container(SlabRows, node, it.cur);

		if (RelFileLocatorEquals(k->locator, rel->rd_locator) && k->part_id == part_id && k->head == d->header_blk && (uint64) k->rb->n == d->nrows)
			return k->rb;
	}
	return NULL;
}

static Snapshot
statement_snapshot(void)
{
	return ActiveSnapshotSet() ? GetActiveSnapshot() : GetTransactionSnapshot();
}

/* The first n rows of rb, in the order they were added */
static SiftRowBuf *
copy_rows(const SiftRowBuf *rb, const SiftPartSpec *spec, int n)
{
	/* check-memory: allow bounded to one micro-slab: SIFT_L0_SLAB_MAX_PAGES pages */
	SiftRowBuf *copy = sift_rowbuf_new(spec);

	for (int r = 0; r < n; r++)
		sift_rowbuf_copy_row(copy, rb, r, rb->xmin[r], rb->cmin[r], rb->subxid[r]);
	return copy;
}

/* The rows of the slab at head; limit >= 0: the first limit of them, in the order they were added, as a process that kept none reads what the slab held when the leader of a scan listed it */
static SiftRowBuf *
slab_rows(Relation rel, const SiftPartSpec *spec, uint64 part_id, BlockNumber head, int limit)
{
	/* check-memory: allow bounded to one micro-slab: SIFT_L0_SLAB_MAX_PAGES pages */
	SiftRowBuf *rb = sift_rowbuf_new(spec);
	SiftRowBuf *kept;

	sift_l0_slab_read(rel, part_id, head, rb, spec, statement_snapshot(), NULL, NULL);
	if (limit >= 0 && rb->n < limit)
		ereport(ERROR, (errcode(ERRCODE_T_R_SERIALIZATION_FAILURE), errmsg("the micro-slab of partition %llu of \"%s\" changed while the statement read it", (unsigned long long) part_id,
																		   RelationGetRelationName(rel)),
						errhint("Run the statement again.")));
	if (limit < 0 || rb->n == limit)
		return rb;
	kept = copy_rows(rb, spec, limit);
	sift_rowbuf_free(rb);
	return kept;
}

/* The part of the rows, which it sorts, or NULL when there is none */
static SiftPart *
rows_part(Relation rel, const SiftPartSpec *spec, uint64 part_id, SiftRowBuf *rb, SiftPartDesc *d)
{
	SiftColIn  *cols;
	SiftRows	r;

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
	const SiftRowBuf *kept = kept_rows(rel, d, part_id);
	SiftRowBuf *rb;
	SiftPartDesc made;
	SiftPart   *part;

	if (kept == NULL)
		SIFT_F2_GATE(SLAB_REREAD_GATE);
	rb = kept != NULL ? copy_rows(kept, &store->spec, kept->n) : slab_rows(rel, &store->spec, part_id, d->header_blk, (int) d->nrows);
	part = rows_part(rel, &store->spec, part_id, rb, &made);

	if (part == NULL)
		ereport(ERROR, (errcode(ERRCODE_T_R_SERIALIZATION_FAILURE), errmsg("the micro-slab of partition %u of \"%s\" is gone", part_id, RelationGetRelationName(rel)),
						errhint("Run the statement again.")));
	return part;
}

/* The descriptor the part of the slab of e has, or false when the statement sees no row in it; the rows are kept in mcxt for the parts made again from it */
static bool
slab_desc(Relation rel, const SiftF2Store *store, const SiftPartitionEntry *e, SiftPartDesc *d, MemoryContext mcxt)
{
	MemoryContext caller = MemoryContextSwitchTo(mcxt);
	SiftRowBuf *rb = slab_rows(rel, &store->spec, e->part_id, e->l0_slab_blk, -1);
	MemoryContext scratch = sift_mem_allocset_create(caller, "sift f2 slab view");
	bool		found;

	MemoryContextSwitchTo(scratch);
	found = rows_part(rel, &store->spec, e->part_id, copy_rows(rb, &store->spec, rb->n), d) != NULL;
	MemoryContextSwitchTo(caller);
	MemoryContextDelete(scratch);
	if (!found)
	{
		sift_rowbuf_free(rb);
		return false;
	}
	keep_rows(rel, e->part_id, e->l0_slab_blk, rb, mcxt);
	d->header_blk = e->l0_slab_blk;
	d->ext_flags |= SIFT_PART_EXT_SLAB;
	return true;
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

		if (snap->partitions[i].l0_slab_blk == InvalidBlockNumber || !slab_desc(rel, store, &snap->partitions[i], &d, mcxt))
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

const SiftPlanSnapshot *
sift_f2_parallel_view(Relation rel, SiftF2Store *store, MemoryContext mcxt)
{
	const SiftPlanSnapshot *snap;
	bool		flushed = false;

	if (RecoveryInProgress())
		return sift_f2_view(rel, store, mcxt);
	snap = sift_plan_cache_get(rel, mcxt);
	for (int i = 0; i < snap->npartitions; i++)
		if (snap->partitions[i].l0_slab_blk != InvalidBlockNumber)
			flushed |= sift_l0_slab_flush(rel, snap->partitions[i].part_id, snap->partitions[i].key_lo, &store->spec);
	return flushed ? sift_plan_cache_get(rel, mcxt) : snap;
}
