/*
 * col/f2/tail.c - the part that holds the end of the file in place. A free run that ends at the high-water mark is cut off the file (sift_fsm_tail_shrink), but a live holder of blocks there
 * pins the mark above the space the ones below it left: a base part, a scope's summary, a projection part, or the pages of a projection directory. When that space is at least
 * SIFT_TAIL_RELOCATE_PCT of the file and the free extents hold the holder (f2_tail_plan.h), it is rewritten into free extents below it that sift_fsm_alloc_below reserves (a base part by the
 * copy on write of a compaction, a projection part by a copy of its granules, a directory by the commit that rewrites its pages), the old blocks are released, and the free run that is left
 * at the end goes with the next cut. A pass starts with a cut, which matures what a compaction just freed (under the lock of the cut no reader of an older manifest can be there), then
 * moves the holders of a plan one after the other, and plans again for as long as the file ends shorter. The metadata slab that ends the file is a holder too: its pages in use (part headers, the
 * manifest, the lists of the allocator) are taken down into the slabs below (rehome_run) before each plan, so that the slab goes with the next cut.
 */
#include "core/host.h"

#include "col/f2/compact/compact.h"
#include "col/f2/f2.h"
#include "col/f2/proj/proj.h"
#include "col/f2/scope/scope.h"
#include "col/f2/store/store.h"
#include "col/f2/tail_plan.h"
#include "col/f2/part/format.h"
#include "core/f2/fsm/fsm.h"
#include "core/maint/maint.h"
#include "core/memtrack.h"
#include "core/page_io.h"
#include "core/restrict.h"

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
	BlockNumber high_water;
	BlockNumber slab_lo;		/* the first block of the metadata slabs that end the file, high_water when none does */
	bool		slab_pins;		/* slabs end the file and their pages in use do not fit in the free pages of the slabs below */
	SiftTailSpan *span;
	TailItem   *item;
	SiftTailPart *part;			/* of each holder: what a move of the pieces of its stream needs */
	int			n;
	int			cap;
} Tail;

/* A slab goes back to the file when no page of it is in use, so a holder with a page in the slabs that end the file holds the end of the file, whatever block of them its page is */
static BlockNumber
slab_run_start(Relation rel, BlockNumber high_water)
{
	BlockNumber lo = high_water;

	while (lo > 0 && sift_fsm_is_meta_block(rel, lo - 1))
		lo--;
	return lo;
}

/* The end of the blocks [start, start + nblocks) as the file is held by them: those of the slabs that end the file hold all of it */
static BlockNumber
holder_hi(const Tail *t, BlockNumber start, uint32 nblocks)
{
	return start >= t->slab_lo ? t->high_water : start + nblocks;
}

static void
tail_add(Relation rel, Tail *t, SiftTailKind kind, TailItem it, const SiftExtent *ext, int n, uint64 reserve, SiftTailPart part)
{
	SiftTailSpan s = {.kind = kind,.lo = PG_UINT32_MAX};

	for (int i = 0; i < n; i++)
	{
		s.lo = Min(s.lo, ext[i].start);
		s.hi = Max(s.hi, holder_hi(t, ext[i].start, ext[i].nblocks));
		if (!sift_fsm_is_meta_block(rel, ext[i].start))
			s.blocks += ext[i].nblocks;
	}
	s.blocks = Max(s.blocks, reserve);
	if (t->n == t->cap)
	{
		t->cap = Max(64, t->cap * 2);
		t->span = t->n == 0 ? MemoryContextAlloc(t->cx, t->cap * sizeof(SiftTailSpan)) : repalloc(t->span, t->cap * sizeof(SiftTailSpan));
		t->item = t->n == 0 ? MemoryContextAlloc(t->cx, t->cap * sizeof(TailItem)) : repalloc(t->item, t->cap * sizeof(TailItem));
		t->part = t->n == 0 ? MemoryContextAlloc(t->cx, t->cap * sizeof(SiftTailPart)) : repalloc(t->part, t->cap * sizeof(SiftTailPart));
	}
	if (part.nstream > 0)
		part.stream = memcpy(MemoryContextAlloc(t->cx, part.nstream * sizeof(SiftExtent)), part.stream, part.nstream * sizeof(SiftExtent));
	t->span[t->n] = s;
	t->part[t->n] = part;
	t->item[t->n++] = it;
	MemoryContextReset(t->scan);
}

/* A part with a stream of its own in blocks can have the pieces of it moved: where its header and extent list lie, and its deletion vector, hold the file at the end they reach. A part with no stream (all of it in its header) has a header to move and no pieces */
static SiftTailPart
movable_part(const Tail *t, const SiftPart *p, const SiftPartDesc *d)
{
	SiftTailPart m = {0};

	if (p == NULL || p->is_packed)
		return m;
	m.stream = p->extents;
	m.nstream = (int) p->hdr.total_extents;
	m.meta_hi = holder_hi(t, p->header_blk, 1);
	for (int i = 0; i < p->nextlist_pages; i++)
		m.meta_hi = Max(m.meta_hi, holder_hi(t, p->extlist_pages[i], 1));
	m.dv_hi = d->dv_blk == InvalidBlockNumber ? 0 : holder_hi(t, d->dv_blk, d->dv_nblocks);
	return m;
}

static void
add_part(Relation rel, Tail *t, const SiftPartitionEntry *e, const SiftPartDesc *d)
{
	MemoryContext old = MemoryContextSwitchTo(t->scan);
	bool		remote = (d->ext_flags & SIFT_PART_EXT_REMOTE) != 0;
	int			n = 0;
	SiftPart   *p = remote ? NULL : sift_part_open_desc(rel, d, (uint32) e->part_id);
	SiftExtent *ext = remote ? palloc(sizeof(SiftExtent)) : sift_part_blocks(p, &n);
	SiftTailPart m = movable_part(t, p, d);

	if (d->dv_blk != InvalidBlockNumber)
	{
		ext = repalloc(ext, (n + 1) * sizeof(SiftExtent));
		ext[n++] = (SiftExtent) {d->dv_blk, d->dv_nblocks};
	}
	MemoryContextSwitchTo(old);
	tail_add(rel, t, TAIL_PART, (TailItem) {.entry = e,.gen = d->part_gen}, ext, n, 0, m);
}

static void
add_dir(Relation rel, Tail *t, uint16 proj_id, const BlockNumber *blk, int n, int nref_blocks)
{
	SiftExtent *ext = palloc(Max(n, 1) * sizeof(SiftExtent));

	for (int i = 0; i < n; i++)
		ext[i] = (SiftExtent) {blk[i], 1};
	tail_add(rel, t, TAIL_PROJ_DIR, (TailItem) {.proj_id = proj_id}, ext, n, (uint64) n + nref_blocks, (SiftTailPart) {0});
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
			tail_add(rel, t, TAIL_PROJ_PART, (TailItem) {.proj_id = refs[i].proj_id,.gen = parts.parts[k].part_gen}, ext, n, 0, (SiftTailPart) {0});
		}
		add_dir(rel, t, refs[i].proj_id, blk, nb, nref_blocks);
	}
}

static Tail
collect(Relation rel, const SiftColMetaPageData *meta, BlockNumber slab_lo)
{
	SiftManifestRead mr = sift_manifest_read_range(rel, NULL, NULL);
	Tail		t = {.cx = CurrentMemoryContext,.scan = sift_mem_allocset_create(CurrentMemoryContext, "sift tail holder"),
		.high_water = meta->high_water,.slab_lo = slab_lo};
	int			nscope = sift_f2_store_open(rel)->host->host_kind == SIFT_F2_HOST_INDEX ? sift_col_rel(rel)->nscope : 0;

	t.slab_pins = t.slab_lo < t.high_water && sift_fsm_meta_short_below(rel, t.slab_lo, false) > 0;

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
			tail_add(rel, &t, TAIL_SCOPE, (TailItem) {.scope_ord = s}, ext, n, 0, (SiftTailPart) {0});
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

/* The part as the manifest holds it now (NULL when it is gone), with the entry it is in */
static SiftPartDesc *
find_part(Relation rel, const TailItem *it, SiftManifestRead *mr, int *at)
{
	int			np;
	SiftPartDesc *parts;

	*mr = sift_manifest_read_range(rel, it->entry->key_lo, it->entry->key_hi);
	if (mr->n == 0 || mr->entries[0].part_id != it->entry->part_id)
		return NULL;
	parts = sift_manifest_read_parts(rel, &mr->entries[0], &np);
	for (*at = 0; *at < np; (*at)++)
		if (parts[*at].part_gen == it->gen)
			return parts;
	return NULL;
}

/* The new header takes the place of the old in the descriptor of the part, unless the part was compacted, dropped or given a new deletion vector since the move was planned: then the blocks written go back */
static bool
publish_move(Relation rel, const TailItem *it, const SiftPartDesc *planned, const SiftPartMove *mv)
{
	uint64		part_id = it->entry->part_id;
	SiftManifestRead mr;
	SiftPartDesc *parts,
				upd;
	SiftManifestEdit e;
	int			at;

	LockPage(rel, SIFT_PART_LOCK_BLKNO(part_id), ExclusiveLock);
	parts = find_part(rel, it, &mr, &at);
	if (parts == NULL || parts[at].header_blk != planned->header_blk || parts[at].dv_blk != planned->dv_blk || parts[at].dv_gen != planned->dv_gen)
	{
		UnlockPage(rel, SIFT_PART_LOCK_BLKNO(part_id), ExclusiveLock);
		sift_fsm_release(rel, mv->made, mv->nmade, InvalidFullTransactionId);
		return false;
	}
	upd = parts[at];
	upd.header_blk = mv->header_blk;
	e = (SiftManifestEdit) {.part_id = part_id,.key_lo = mr.entries[0].key_lo,.update_parts = &upd,.expect_parts = &parts[at],.nupdate = 1,.release = mv->freed,.nrelease = mv->nfreed};
	sift_manifest_commit(rel, &e);
	UnlockPage(rel, SIFT_PART_LOCK_BLKNO(part_id), ExclusiveLock);
	return true;
}

#define HEADER_ONLY		MaxBlockNumber	/* a cut that no piece of a stream reaches: the move gives the part a new header and extent list and nothing else */

typedef enum PiecesMove
{
	PIECES_MOVED,
	PIECES_NO_ROOM,				/* the free space below the cut is not in extents that take the pieces */
	PIECES_SKIPPED				/* the part is busy, gone or not one to move */
} PiecesMove;

/* Under the lock of the compaction of the partition, which would free the blocks of the part meanwhile */
static PiecesMove
move_pieces_locked(Relation rel, const TailItem *it, BlockNumber cut)
{
	SiftManifestRead mr;
	int			at,
				nto = 0;
	SiftPartDesc *parts = find_part(rel, it, &mr, &at),
				planned;
	SiftPart   *part;
	SiftExtent *to = NULL;
	SiftPartMove mv;
	uint64		want;

	if (parts == NULL || (parts[at].flags & SIFT_PART_FLAG_PACKED_SLOT) || (parts[at].ext_flags & SIFT_PART_EXT_REMOTE))
		return PIECES_SKIPPED;
	planned = parts[at];
	part = sift_part_open_desc(rel, &planned, (uint32) it->entry->part_id);
	want = sift_tail_blocks_from(part->extents, part->hdr.total_extents, cut);
	if (want == 0 && cut != HEADER_ONLY)
		return PIECES_SKIPPED;
	if (want > 0)
	{
		to = palloc(Min(want, (uint64) SIFT_TAIL_MAX_PIECES) * sizeof(SiftExtent));
		nto = sift_fsm_alloc_below(rel, want, cut, to, (int) Min(want, (uint64) SIFT_TAIL_MAX_PIECES));
		if (nto == 0)
			return PIECES_NO_ROOM;
	}
	mv = sift_part_move_tail(rel, part, cut, to, nto);
	return publish_move(rel, it, &planned, &mv) ? PIECES_MOVED : PIECES_SKIPPED;
}

/*
 * The pieces of the stream of a part that lie at or above the cut are copied into the free blocks below it, as they are, under the generation they have (no page is decoded or encoded, the
 * descriptor keeps its generation and takes a new header): the part is not rewritten whole, which a part larger than the free space below it, or a free space in many pieces, could not be.
 */
static PiecesMove
move_pieces(Relation rel, const TailItem *it, BlockNumber cut)
{
	uint64		part_id = it->entry->part_id;
	PiecesMove	moved;

	if (!ConditionalLockPage(rel, SIFT_COMPACT_LOCK_BLKNO(part_id), ExclusiveLock))
		return PIECES_SKIPPED;
	moved = move_pieces_locked(rel, it, cut);
	UnlockPage(rel, SIFT_COMPACT_LOCK_BLKNO(part_id), ExclusiveLock);
	return moved;
}

/* The highest holder that has stream at or above the cut; -1 when none has */
static int
top_above(const Tail *t, BlockNumber cut)
{
	int			top = -1;
	BlockNumber hi = cut;

	for (int i = 0; i < t->n; i++)
		if (t->part[i].nstream > 0 && sift_tail_stream_hi(&t->part[i]) > hi)
		{
			top = i;
			hi = sift_tail_stream_hi(&t->part[i]);
		}
	return top;
}

/* One holder's pieces moved to where the file can end at the lowest cut that the free space takes; a cut it is too scattered for is raised until the file would gain no more than it must */
static bool
relocate_pieces(Relation rel, const Tail *t, BlockNumber hw, uint64 free_blocks)
{
	BlockNumber cut,
				from = 0;

	while (sift_tail_pick_cut(t->span, t->part, t->n, hw, t->slab_pins, free_blocks, SIFT_TAIL_SHRINK_MIN_BLOCKS, from, &cut))
	{
		int			top = top_above(t, cut);
		PiecesMove	moved = top < 0 ? PIECES_SKIPPED : move_pieces(rel, &t->item[top], cut);

		if (moved != PIECES_NO_ROOM)
			return moved == PIECES_MOVED;
		from = cut + (hw - cut + 1) / 2;
	}
	return false;
}

static bool
relocate_top(Relation rel, const SiftF2Store *store, FullTransactionId horizon, int64 hot_cut, int *left)
{
	SiftColMetaPageData meta = sift_col_meta_read(rel);
	Tail		t = collect(rel, &meta, slab_run_start(rel, meta.high_water));
	SiftFsmStats st = sift_fsm_check(rel);
	uint64		free_blocks = 0;
	SiftTailPick pick;

	for (int k = 0; k < SIFT_FSM_NCLASSES; k++)
		free_blocks += st.class_blocks[k];
	if (sift_tail_pick(t.span, t.n, meta.high_water, free_blocks, SIFT_TAIL_SHRINK_MIN_BLOCKS, &pick))
	{
		*left = *left < 0 ? pick.run : *left;
		if (*left == 0)
			return false;
		if (move_below(rel, store, &t.span[pick.top], &t.item[pick.top], &pick, horizon, hot_cut))
		{
			(*left)--;
			return true;
		}
	}
	return relocate_pieces(rel, &t, meta.high_water, free_blocks);
}

/* The holders of one plan are moved one after the other, the plan asked again after each; whether the file ended shorter */
static bool
relocate_run(Relation rel, const SiftF2Store *store, FullTransactionId horizon, int64 hot_cut, MemoryContext pass)
{
	BlockNumber high_water = sift_col_meta_read(rel).high_water;
	int			left = -1;

	while (relocate_top(rel, store, horizon, hot_cut, &left))
	{
		(void) sift_fsm_tail_shrink(rel);
		MemoryContextReset(pass);
	}
	(void) sift_fsm_tail_shrink(rel);
	return sift_col_meta_read(rel).high_water < high_water;
}

/* The parts whose header or extent list lies in the slabs that end the file are given new ones, which the commit puts in the lowest free pages of the slabs; the pages it replaced are reclaimed at once, so that the next commit finds them free */
static void
rehome_headers(Relation rel, const Tail *t)
{
	MemoryContext move = sift_mem_allocset_create(CurrentMemoryContext, "sift tail header move");
	MemoryContext old = MemoryContextSwitchTo(move);

	for (int i = 0; i < t->n; i++)
	{
		if (t->span[i].kind == TAIL_PART && t->part[i].meta_hi == t->high_water &&
			move_pieces(rel, &t->item[i], HEADER_ONLY) == PIECES_MOVED)
			(void) sift_fsm_reclaim_exclusive(rel);
		MemoryContextReset(move);
	}
	MemoryContextSwitchTo(old);
	MemoryContextDelete(move);
}

static void
rehome_manifest(Relation rel, BlockNumber lo)
{
	SiftManifestRead mr = sift_manifest_pages_from(rel, lo);

	for (int i = 0; i < mr.n; i++)
	{
		(void) sift_manifest_commit(rel, &(SiftManifestEdit) {.part_id = mr.entries[i].part_id,.key_lo = mr.entries[i].key_lo,.relist = true});
		(void) sift_fsm_reclaim_exclusive(rel);
	}
}

/*
 * The pages in use of the slab that ends the file are written below it, in room that the free extents under it give when the slabs there are full: the part headers, the manifest and the
 * lists of the allocator. A slab with no page in use that the slabs below cannot spare the reserve for is given that room too. The relation is held, so that no reader keeps the pages replaced.
 */
static bool
rehome_slabs(Relation rel)
{
	SiftColMetaPageData meta = sift_col_meta_read(rel);
	BlockNumber top = sift_fsm_slab_start_ending_at(rel, meta.high_water);
	Tail		t;

	if (top == InvalidBlockNumber || (sift_fsm_meta_used_from(rel, top) == 0 && sift_fsm_meta_short_below(rel, top, true) == 0))
		return false;
	t = collect(rel, &meta, top);
	if (!sift_fsm_slab_room_below(rel, top))
		return false;
	rehome_headers(rel, &t);
	rehome_manifest(rel, top);
	(void) sift_fsm_rehome_lists(rel, top);
	return true;
}

/* The slabs are emptied, and the ones left with no page in use are cut off with the free run behind them */
static void
rehome_run(Relation rel)
{
	if (sift_maint_standby_streams() || !ConditionalLockRelation(rel, AccessExclusiveLock))
		return;
	if (rehome_slabs(rel))
	{
		(void) sift_fsm_tail_shrink(rel);
		(void) sift_fsm_tail_shrink(rel);
	}
	UnlockRelation(rel, AccessExclusiveLock);
}

/* The slabs that end the file emptied and then the holders of its end moved, as the move of the one frees what pins the other: whether the file ended shorter */
static bool
tail_pass(Relation rel, const SiftF2Store *store, FullTransactionId horizon, int64 hot_cut, MemoryContext pass)
{
	BlockNumber high_water = sift_col_meta_read(rel).high_water;

	CHECK_FOR_INTERRUPTS();
	rehome_run(rel);
	MemoryContextReset(pass);
	(void) relocate_run(rel, store, horizon, hot_cut, pass);
	return sift_col_meta_read(rel).high_water < high_water;
}

void
sift_f2_tail_relocate(Relation rel, const SiftF2Store *store, FullTransactionId horizon, int64 hot_cut)
{
	MemoryContext pass;
	MemoryContext old;

	/* The locks of the pass are WAL-logged for a standby, which takes a transaction id */
	if (sift_xid_unassignable())
		return;
	pass = sift_mem_allocset_create(CurrentMemoryContext, "sift tail relocate");
	old = MemoryContextSwitchTo(pass);
	(void) sift_fsm_tail_shrink(rel);
	while (tail_pass(rel, store, horizon, hot_cut, pass))
		;
	MemoryContextSwitchTo(old);
	sift_mem_release_os(MemoryContextMemAllocated(pass, true));
	MemoryContextDelete(pass);
}
