/*
 * col/f2/f2_tail.c - the part that holds the end of the file in place. A free run that ends at the high-water mark is cut off the file (sift_fsm_tail_shrink), but a live holder of blocks there
 * pins the mark above the space the ones below it left: a base part, a scope's summary, a projection part, or the pages of a projection directory. When that space is at least
 * SIFT_TAIL_RELOCATE_PCT of the file and the free extents hold the holder (f2_tail_plan.h), it is rewritten into free extents below it that sift_fsm_alloc_below reserves (a base part by the
 * copy on write of a compaction, a projection part by a copy of its granules, a directory by the commit that rewrites its pages), the old blocks are released, and the free run that is left
 * at the end goes with the next cut. A pass starts with a cut, which matures what a compaction just freed (under the lock of the cut no reader of an older manifest can be there), and moves up to
 * SIFT_TAIL_MAX_MOVES holders.
 */
#include "core/host.h"

#include "col/f2/compact.h"
#include "col/f2/f2.h"
#include "col/f2/f2_proj.h"
#include "col/f2/f2_scope.h"
#include "col/f2/f2_store.h"
#include "col/f2/f2_tail_plan.h"
#include "col/f2/part_format.h"
#include "core/f2/fsm.h"
#include "core/memtrack.h"
#include "core/page_io.h"

typedef struct TailItem
{
	const SiftPartitionEntry *entry;	/* TAIL_PART */
	uint64		gen;			/* TAIL_PART, TAIL_PROJ_PART */
	int			scope_ord;		/* TAIL_SCOPE */
	uint16		proj_id;		/* TAIL_PROJ_PART, TAIL_PROJ_DIR */
} TailItem;

/* The holders found: the spans and the items are kept in cx; what one holder is read with (its opened part, its extents) lives in scan until the next holder, so that the parts of the relation are not open together */
typedef struct Tail
{
	MemoryContext cx;
	MemoryContext scan;
	SiftTailSpan *span;
	TailItem   *item;
	int			n;
	int			cap;
} Tail;

static void
tail_add(Relation rel, Tail *t, SiftTailKind kind, TailItem it, const SiftExtent *ext, int n, uint64 reserve)
{
	SiftTailSpan s = {.kind = kind,.lo = PG_UINT32_MAX};

	for (int i = 0; i < n; i++)
	{
		s.lo = Min(s.lo, ext[i].start);
		s.hi = Max(s.hi, ext[i].start + ext[i].nblocks);
		if (!sift_fsm_is_meta_block(rel, ext[i].start))
			s.blocks += ext[i].nblocks;
	}
	s.blocks = Max(s.blocks, reserve);
	if (t->n == t->cap)
	{
		t->cap = Max(64, t->cap * 2);
		t->span = t->n == 0 ? MemoryContextAlloc(t->cx, t->cap * sizeof(SiftTailSpan)) : repalloc(t->span, t->cap * sizeof(SiftTailSpan));
		t->item = t->n == 0 ? MemoryContextAlloc(t->cx, t->cap * sizeof(TailItem)) : repalloc(t->item, t->cap * sizeof(TailItem));
	}
	t->span[t->n] = s;
	t->item[t->n++] = it;
	MemoryContextReset(t->scan);
}

static void
add_part(Relation rel, Tail *t, const SiftPartitionEntry *e, const SiftPartDesc *d)
{
	MemoryContext old = MemoryContextSwitchTo(t->scan);
	bool		remote = (d->ext_flags & SIFT_PART_EXT_REMOTE) != 0;
	int			n = 0;
	SiftExtent *ext = remote ? palloc(sizeof(SiftExtent)) : sift_part_blocks(sift_part_open_desc(rel, d, (uint32) e->part_id), &n);

	if (d->dv_blk != InvalidBlockNumber)
	{
		ext = repalloc(ext, (n + 1) * sizeof(SiftExtent));
		ext[n++] = (SiftExtent) {d->dv_blk, d->dv_nblocks};
	}
	MemoryContextSwitchTo(old);
	tail_add(rel, t, TAIL_PART, (TailItem) {.entry = e,.gen = d->part_gen}, ext, n, 0);
}

static void
add_dir(Relation rel, Tail *t, uint16 proj_id, const BlockNumber *blk, int n, int nref_blocks)
{
	SiftExtent *ext = palloc(Max(n, 1) * sizeof(SiftExtent));

	for (int i = 0; i < n; i++)
		ext[i] = (SiftExtent) {blk[i], 1};
	tail_add(rel, t, TAIL_PROJ_DIR, (TailItem) {.proj_id = proj_id}, ext, n, (uint64) n + nref_blocks);
}

/* The parts of every projection and the pages of its directory; the pages of the refs are one holder, owned by the first projection whose rewrite carries them */
static void
add_projections(Relation rel, Tail *t, const SiftColMetaPageData *meta)
{
	int			nrefs,
				nref_blocks;
	SiftManifestProjRef *refs = sift_manifest_read_projs(rel, meta, &nrefs);
	BlockNumber *ref_blocks = sift_proj_ref_dir_blocks(rel, meta, &nref_blocks);

	if (nrefs > 0)
		add_dir(rel, t, refs[0].proj_id, ref_blocks, nref_blocks, 0);
	for (int i = 0; i < nrefs; i++)
	{
		SiftProjParts parts = sift_manifest_read_proj_parts(rel, &refs[i]);
		int			nb;
		BlockNumber *blk = sift_proj_one_dir_blocks(rel, &refs[i], &nb);

		for (int k = 0; k < parts.nparts; k++)
		{
			int			n;
			MemoryContext old = MemoryContextSwitchTo(t->scan);
			SiftExtent *ext = sift_proj_part_extents(rel, &parts.parts[k], &n);

			MemoryContextSwitchTo(old);
			tail_add(rel, t, TAIL_PROJ_PART, (TailItem) {.proj_id = refs[i].proj_id,.gen = parts.parts[k].part_gen}, ext, n, 0);
		}
		add_dir(rel, t, refs[i].proj_id, blk, nb, nref_blocks);
	}
}

static Tail
collect(Relation rel, const SiftColMetaPageData *meta)
{
	SiftManifestRead mr = sift_manifest_read_range(rel, NULL, NULL);
	Tail		t = {.cx = CurrentMemoryContext,.scan = sift_mem_allocset_create(CurrentMemoryContext, "sift tail holder")};
	int			nscope = sift_f2_store_open(rel)->host->host_kind == SIFT_F2_HOST_INDEX ? sift_col_rel(rel)->nscope : 0;

	for (int i = 0; i < mr.n; i++)
	{
		int			np;
		SiftPartDesc *parts = sift_manifest_read_parts(rel, &mr.entries[i], &np);

		for (int p = 0; p < np; p++)
			add_part(rel, &t, &mr.entries[i], &parts[p]);
		pfree(parts);
	}
	for (int s = 0; s < nscope; s++)
	{
		SiftExtent *ext;
		int			n;

		if (sift_f2_scope_blocks(rel, s, &ext, &n))
			tail_add(rel, &t, TAIL_SCOPE, (TailItem) {.scope_ord = s}, ext, n, 0);
	}
	if (meta->nprojs > 0)
		add_projections(rel, &t, meta);
	MemoryContextDelete(t.scan);
	return t;
}

static void
rewrite(Relation rel, const SiftF2Store *store, SiftTailKind kind, const TailItem *it, FullTransactionId horizon, int64 hot_cut)
{
	SiftCompactResult res;
	uint64		gen = it->gen;

	switch (kind)
	{
		case TAIL_PART:
			(void) sift_partition_compact_online(rel, it->entry->part_id, it->entry->key_lo, &gen, 1, &store->spec, hot_cut, horizon, &res);
			break;
		case TAIL_SCOPE:
			sift_f2_scope_move(rel, it->scope_ord);
			break;
		case TAIL_PROJ_PART:
			(void) sift_proj_move_part(rel, it->proj_id, it->gen);
			break;
		case TAIL_PROJ_DIR:
			(void) sift_proj_move_dir(rel, it->proj_id);
			break;
	}
}

/* The holder is rewritten into blocks below max_blk, which the allocator reserves first: when the free space under it does not hold it, the file would be no shorter and nothing is rewritten */
static bool
move_below(Relation rel, const SiftF2Store *store, const SiftTailSpan *span, const TailItem *it, const SiftTailPick *pick, FullTransactionId horizon, int64 hot_cut)
{
	int			max = (int) (span->blocks / SIFT_FSM_MAX_EXTENT + SIFT_FSM_NCLASSES + 1);
	SiftExtent *ext = palloc(max * sizeof(SiftExtent));
	int			n = sift_fsm_alloc_below(rel, span->blocks, pick->below_part, ext, max);

	if (n == 0)
		n = sift_fsm_alloc_below(rel, span->blocks, pick->below_cut, ext, max);
	if (n == 0)
		return false;
	sift_part_prefer_extents(rel, ext, n);
	PG_TRY();
	{
		rewrite(rel, store, span->kind, it, horizon, hot_cut);
	}
	PG_FINALLY();
	{
		sift_part_release_preferred(rel);
	}
	PG_END_TRY();
	return true;
}

static bool
relocate_top(Relation rel, const SiftF2Store *store, FullTransactionId horizon, int64 hot_cut)
{
	SiftColMetaPageData meta = sift_col_meta_read(rel);
	Tail		t = collect(rel, &meta);
	SiftFsmStats st = sift_fsm_check(rel);
	uint64		free_blocks = 0;
	SiftTailPick pick;

	for (int k = 0; k < SIFT_FSM_NCLASSES; k++)
		free_blocks += st.class_blocks[k];
	return sift_tail_pick(t.span, t.n, meta.high_water, free_blocks, SIFT_TAIL_SHRINK_MIN_BLOCKS, &pick) &&
		move_below(rel, store, &t.span[pick.top], &t.item[pick.top], &pick, horizon, hot_cut);
}

void
sift_f2_tail_relocate(Relation rel, const SiftF2Store *store, FullTransactionId horizon, int64 hot_cut)
{
	MemoryContext pass = sift_mem_allocset_create(CurrentMemoryContext, "sift tail relocate");
	MemoryContext old = MemoryContextSwitchTo(pass);

	(void) sift_fsm_tail_shrink(rel);
	for (int moved = 0; moved < SIFT_TAIL_MAX_MOVES && relocate_top(rel, store, horizon, hot_cut); moved++)
	{
		(void) sift_fsm_tail_shrink(rel);
		MemoryContextReset(pass);
	}
	MemoryContextSwitchTo(old);
	sift_mem_release_os(MemoryContextMemAllocated(pass, true));
	MemoryContextDelete(pass);
}
