/*
 * core/f2/fsm/slab.c - the metadata slabs of the allocator of format major 2 (core/f2/fsm/fsm.h): the slabs of the root, the claim of a slab, and the way back: pages of the queue that matured are
 * reclaimed before a slab is claimed, and a slab whose every block is free goes back to the data zone (core/f2/fsm/chain.c, which keeps the bitmaps of the root and of its chain), so that the number
 * of slabs follows the live metadata and not the history of the writes.
 */
#include "core/host.h"

#include "col/f2/meta.h"
#include "core/f2/fsm/fsm.h"
#include "core/f2/fsm/int.h"
#include "core/page_io.h"

void
sift_fsm_slab_init(SiftFsmSlab *s, BlockNumber start, uint32 nblocks)
{
	s->start = start;
	s->nblocks = nblocks;
	memset(s->free, 0, sizeof(s->free));
	for (uint32 b = 0; b < nblocks; b++)
		s->free[b / 64] |= UINT64CONST(1) << (b % 64);
}

/* In address order after the first slab, so that the slabs a quiet relation keeps are the lowest ones and the empty slab given back first is the highest */
void
sift_fsm_slab_insert(SiftFsmSlabTable *t, BlockNumber start, uint32 nblocks)
{
	uint32		at = t->nslabs;

	while (at > 1 && t->slab[at - 1].start > start)
		at--;
	memmove(&t->slab[at + 1], &t->slab[at], (t->nslabs - at) * sizeof(SiftFsmSlab));
	sift_fsm_slab_init(&t->slab[at], start, nblocks);
	t->nslabs++;
}

void
sift_fsm_slab_add(Page root, BlockNumber start, uint32 nblocks)
{
	sift_fsm_slab_insert(SLABS(root), start, nblocks);
	FSMM(root)->meta_free_count += nblocks;
}

const SiftFsmSlab *
sift_fsm_slab_ending_at(const SiftFsmSlabTable *t, BlockNumber end)
{
	for (uint32 i = 1; i < t->nslabs; i++)
		if (t->slab[i].start + t->slab[i].nblocks == end)
			return &t->slab[i];
	return NULL;
}

uint32
sift_fsm_slab_used(const SiftFsmSlab *s)
{
	uint32		free = 0;

	for (int w = 0; w < SIFT_FSM_META_SLAB / 64; w++)
		free += pg_popcount64(s->free[w]);
	return s->nblocks - free;
}

/* The blocks of the slab up to and with its last page in use: what the slab cannot be cut below */
static uint32
used_prefix(const SiftFsmSlab *s)
{
	uint32		end = 0;

	for (uint32 b = 0; b < s->nblocks; b++)
		if ((s->free[b / 64] & (UINT64CONST(1) << (b % 64))) == 0)
			end = b + 1;
	return end;
}

static int
slab_start_desc(const void *a, const void *b)
{
	BlockNumber x = (*(const SiftFsmSlab *const *) a)->start,
				y = (*(const SiftFsmSlab *const *) b)->start;

	return x < y ? 1 : x > y ? -1 : 0;
}

/*
 * The blocks of the slabs that are free space of the file, the highest slab first: a slab with no page in use goes whole when the others keep the reserve free, and any slab gives up the
 * blocks behind the pages it holds (and behind the free blocks of its own that the reserve still needs), keeping a power of two of blocks, as every slab has. The first slab holds the root of the partition
 * directory and stays whole. A slab claimed in a burst is as big as the burst needed, so one that the burst left with a few pages, or with nothing but the reserve, is cut down to what they take.
 */
void
sift_fsm_slabs_idle(const SiftFsmSlabTable *t, uint32 meta_free, ExtVec *out)
{
	const SiftFsmSlab *order[SIFT_FSM_ROOT_SLABS];
	int			n = 0;
	uint32		left = t->nslabs;

	for (uint32 i = 1; i < t->nslabs; i++)
		order[n++] = &t->slab[i];
	qsort(order, n, sizeof(order[0]), slab_start_desc);
	for (int i = 0; i < n; i++)
	{
		const SiftFsmSlab *s = order[i];
		uint32		used = sift_fsm_slab_used(s),
					elsewhere = meta_free - (s->nblocks - used),
					need = sift_fsm_meta_reserve(left) > elsewhere ? sift_fsm_meta_reserve(left) - elsewhere : 0,
					keep;

		if (used == 0 && elsewhere >= sift_fsm_meta_reserve(left - 1))
		{
			sift_fsm_extvec_add(out, s->start, s->nblocks, InvalidFullTransactionId);
			meta_free -= s->nblocks;
			left--;
			continue;
		}
		keep = Min(s->nblocks, pg_nextpower2_32(Max(SIFT_FSM_SLAB_FIRST, Max(used_prefix(s), used + need))));
		if (keep < s->nblocks)
		{
			sift_fsm_extvec_add(out, s->start + keep, s->nblocks - keep, InvalidFullTransactionId);
			meta_free -= s->nblocks - keep;
		}
	}
}

/* Takes the blocks that idle names from the slabs of the root page: a slab that idle names from its first block leaves the table, one that it names the end of is cut to what is before it */
void
sift_fsm_slabs_drop(Page root, const ExtVec *idle)
{
	SiftFsmSlabTable *t = SLABS(root);
	uint32		kept = 0;

	if (idle->n == 0)
		return;
	for (uint32 i = 0; i < t->nslabs; i++)
	{
		SiftFsmSlab *s = &t->slab[i];
		const SiftFsmExtent *cut = NULL;

		for (int k = 0; k < idle->n && cut == NULL; k++)
			if (idle->v[k].start >= s->start && idle->v[k].start < s->start + s->nblocks)
				cut = &idle->v[k];
		if (cut == NULL)
			t->slab[kept++] = *s;
		else
		{
			FSMM(root)->meta_free_count -= cut->nblocks;
			if (cut->start > s->start)
			{
				s->nblocks = cut->start - s->start;
				for (uint32 b = s->nblocks; b < SIFT_FSM_META_SLAB; b++)
					s->free[b / 64] &= ~(UINT64CONST(1) << (b % 64));
				t->slab[kept++] = *s;
			}
		}
	}
	memset(&t->slab[kept], 0, (t->nslabs - kept) * sizeof(SiftFsmSlab));
	t->nslabs = kept;
}

static bool
queued(FsmCtx *c)
{
	Buffer		rootbuf = sift_fsm_lock_root(c->rel);
	bool		q = FSMM(BufferGetPage(rootbuf))->quarantine_extents > 0;

	UnlockReleaseBuffer(rootbuf);
	return q;
}

static void
reclaim_before_claim(FsmCtx *c)
{
	if (!c->in_reclaim && queued(c))
		(void) sift_fsm_reclaim_locked(c, NULL);
}

/*
 * A new slab: a free extent of its class when there is one, else the end of the relation; never a split, which would need a page of a list. The extent leaves its list in the record that makes it the
 * slab, so no error or crash finds it in neither; the list page it empties goes back in that record where the record has room for its page of the chain, else in the next (sift_fsm_slab_mark)
 */
void
sift_fsm_claim_slab(FsmCtx *c)
{
	SiftXlog	x = sift_xlog_start(c->rel);
	Page		root = sift_xlog_register(&x, sift_fsm_lock_root(c->rel), 0);
	uint32		nblocks = SIFT_FSM_SLAB_BLOCKS(SLABS(root)->nslabs);
	SiftFsmExtent e = {.start = InvalidBlockNumber};
	BlockNumber emptied = InvalidBlockNumber;
	SiftFsmSlabDir d;

	if (!sift_fsm_pop_class_in(c, &x, root, pg_ceil_log2_32(nblocks), &e, &emptied))
	{
		e.start = sift_fsm_high_water(c);
		sift_fsm_set_high_water(c, &x, e.start + nblocks);
	}
	SIFT_POINT("sift-fsm-claim-slab");
	d = sift_fsm_dir_read(c->rel, root, &x);
	sift_fsm_dir_add(&d, c, e.start, nblocks);
	if (emptied != InvalidBlockNumber && sift_fsm_dir_fits(&d, emptied))
	{
		sift_fsm_dir_put(&d, emptied);
		emptied = InvalidBlockNumber;
	}
	sift_fsm_root_seal(root);
	sift_fsm_dir_free(&d);
	sift_fsm_xlog_done(c, &x);
	if (emptied != InvalidBlockNumber)
		sift_fsm_slab_mark(c->rel, &emptied, 1, true);
}

/* Extents the caller took out of the lists (each at most SIFT_FSM_META_SLAB blocks) become metadata slabs, one record each */
void
sift_fsm_adopt_slabs(FsmCtx *c, const SiftExtent *ext, int n)
{
	for (int i = 0; i < n; i++)
	{
		SiftXlog	x = sift_xlog_start(c->rel);
		SiftFsmSlabDir d = sift_fsm_dir_read(c->rel, sift_xlog_register(&x, sift_fsm_lock_root(c->rel), 0), &x);

		sift_fsm_dir_add(&d, c, ext[i].start, ext[i].nblocks);
		sift_fsm_root_seal(d.page[0]);
		sift_fsm_dir_free(&d);
		sift_fsm_xlog_done(c, &x);
	}
}

/* STOR[STOR-F2-03]: FSM small-index meta reserve and multi-extent fallback (cuts EOF growth) */
bool
sift_fsm_below_reserve(FsmCtx *c)
{
	Buffer		rootbuf = sift_fsm_lock_root(c->rel);
	Page		root = BufferGetPage(rootbuf);
	bool		low = FSMM(root)->meta_free_count < sift_fsm_meta_reserve(SLABS(root)->nslabs);

	UnlockReleaseBuffer(rootbuf);
	return low;
}

void
sift_fsm_grow_meta(FsmCtx *c)
{
	sift_fsm_ctx_exclusive(c);
	reclaim_before_claim(c);
	if (sift_fsm_below_reserve(c))
		sift_fsm_claim_slab(c);
}

BlockNumber
sift_fsm_alloc_meta_locked(FsmCtx *c)
{
	for (;;)
	{
		SiftXlog	x = sift_xlog_start(c->rel);
		Buffer		rootbuf = sift_fsm_lock_root(c->rel);
		Page		root = sift_xlog_register(&x, rootbuf, 0);
		BlockNumber blk = sift_fsm_slab_take(c->rel, &x, root);

		if (blk == InvalidBlockNumber)
		{
			sift_xlog_abort(&x);
			UnlockReleaseBuffer(rootbuf);
			sift_fsm_grow_meta(c);
			continue;
		}
		sift_fsm_root_seal(root);
		sift_xlog_done(&x);
		return blk;
	}
}

/*
 * The pages of the slabs for a pool (core/shm/lease.h) in one record of the root: block 0 is held shared, which keeps out every step that plans its changes of the lists and the slabs in
 * more than one look at the root (reclaim, the sweep, the rehome), and the root is held exclusively, so that batches of several backends follow each other. The pages are those of a window of
 * SIFT_FSM_POOL_SPAN blocks of the lowest slab that has a free one, and the root names the window (pool_lo, pool_mask): what a restart or a crash takes from the shared memory of the pool is named
 * there, and the sweep frees it. A pool is filled when it is empty, so the window of the last batch is the only one that can hold pages that nothing uses; a pool that lost its pages (pool_lost)
 * waits for the sweep to free them before it takes the next window (a relation that the sweep cannot name, and the check of the blocks does not judge, takes it at once). The slabs are left with their reserve and one page to spare: a batch never makes the step that holds block 0 exclusively claim a
 * slab, and 0 is returned where it cannot take any page without that.
 */
int
sift_fsm_alloc_meta_batch(Relation rel, BlockNumber *out, bool pool_lost)
{
	Buffer		buf0 = sift_col_meta_lock(rel, BUFFER_LOCK_SHARE);
	SiftXlog	x = sift_xlog_start(rel);
	Buffer		rootbuf = sift_fsm_lock_root(rel);
	Page		root = sift_xlog_register(&x, rootbuf, 0);
	uint32		spare = FSMM(root)->meta_free_count;
	uint32		reserve = sift_fsm_meta_reserve(SLABS(root)->nslabs);
	BlockNumber lo = 0;
	uint32		mask;
	int			got = 0;
	SiftFsmSlabDir d;

	if (spare <= reserve || (pool_lost && FSMM(root)->pool_mask != 0 && sift_fsm_names_all_extents(rel)))
	{
		sift_xlog_abort(&x);
		UnlockReleaseBuffer(rootbuf);
		UnlockReleaseBuffer(buf0);
		return 0;
	}
	d = sift_fsm_dir_read(rel, root, &x);
	(void) sift_fsm_dir_take_window(&d, (int) Min((uint32) SIFT_FSM_POOL_SPAN, spare - reserve), &lo, &mask);
	sift_fsm_dir_free(&d);
	FSMM(root)->pool_lo = lo;
	FSMM(root)->pool_mask = mask;
	for (int i = 0; i < SIFT_FSM_POOL_SPAN; i++)
		if (mask & (UINT32_C(1) << i))
			out[got++] = lo + i;
	sift_fsm_root_seal(root);
	sift_xlog_done(&x);
	UnlockReleaseBuffer(buf0);
	return got;
}

/* The pages of a pool back to the bitmaps of the slabs (block 0 shared, as in sift_fsm_alloc_meta_batch) */
void
sift_fsm_free_meta_batch(Relation rel, const BlockNumber *blks, int n)
{
	Buffer		buf0;

	if (n == 0)
		return;
	buf0 = sift_col_meta_lock(rel, BUFFER_LOCK_SHARE);
	sift_fsm_slab_mark(rel, blks, n, true);
	UnlockReleaseBuffer(buf0);
}
