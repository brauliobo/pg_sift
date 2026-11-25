/*
 * core/f2/fsm/slab.c - the metadata slabs of the allocator of format major 2 (core/f2/fsm.h): the bitmaps, the claim of a slab, and the way back: pages of the queue that matured are reclaimed
 * before a slab is claimed, and a slab whose every block is free goes back to the data zone, so that the number of slabs follows the live metadata and not the history of the writes.
 */
#include "core/host.h"

#include "col/f2/col_meta.h"
#include "core/f2/fsm.h"
#include "core/f2/fsm/fsm_int.h"
#include "core/horizon.h"
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
sift_fsm_slab_add(Page root, BlockNumber start, uint32 nblocks)
{
	SiftFsmSlabTable *t = SLABS(root);
	uint32		at = t->nslabs;

	while (at > 1 && t->slab[at - 1].start > start)
		at--;
	memmove(&t->slab[at + 1], &t->slab[at], (t->nslabs - at) * sizeof(SiftFsmSlab));
	sift_fsm_slab_init(&t->slab[at], start, nblocks);
	t->nslabs++;
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

SiftFsmSlab *
sift_fsm_slab_of(SiftFsmSlabTable *t, BlockNumber blk)
{
	for (uint32 i = 0; i < t->nslabs; i++)
		if (blk >= t->slab[i].start && blk < t->slab[i].start + t->slab[i].nblocks)
			return &t->slab[i];
	return NULL;
}

static bool
has_free(const SiftFsmSlab *s)
{
	for (int w = 0; w < SIFT_FSM_META_SLAB / 64; w++)
		if (s->free[w] != 0)
			return true;
	return false;
}

/* The lowest free block of the slabs, or InvalidBlockNumber when all are full: the metadata stays at the front of the file, so that the slabs behind it can empty */
BlockNumber
sift_fsm_slab_take(Page root)
{
	SiftFsmSlabTable *t = SLABS(root);
	SiftFsmSlab *low = NULL;

	for (uint32 i = 0; i < t->nslabs; i++)
		if (has_free(&t->slab[i]) && (low == NULL || t->slab[i].start < low->start))
			low = &t->slab[i];
	if (low == NULL)
		return InvalidBlockNumber;
	for (int w = 0;; w++)
		if (low->free[w] != 0)
		{
			int			bit = pg_rightmost_one_pos64(low->free[w]);

			low->free[w] &= ~(UINT64CONST(1) << bit);
			FSMM(root)->meta_free_count--;
			return low->start + w * 64 + bit;
		}
}

void
sift_fsm_slab_put(Relation rel, Page root, BlockNumber blk)
{
	SiftFsmSlab *s = sift_fsm_slab_of(SLABS(root), blk);
	uint64		bit;

	if (s == NULL)
		SIFT_CORRUPT(rel, "Block %u is freed as a metadata page and is in no metadata slab.", blk);
	bit = UINT64CONST(1) << ((blk - s->start) % 64);
	if (s->free[(blk - s->start) / 64] & bit)
		SIFT_CORRUPT(rel, "Metadata block %u is freed twice.", blk);
	s->free[(blk - s->start) / 64] |= bit;
	FSMM(root)->meta_free_count++;
}

/* Every block of the slab free: the words of its bitmap, the last one cut to the blocks it has */
static bool
all_free(const SiftFsmSlab *s)
{
	for (uint32 w = 0; w * 64 < s->nblocks; w++)
	{
		uint32		bits = Min(s->nblocks - w * 64, 64);
		uint64		want = bits == 64 ? ~UINT64CONST(0) : (UINT64CONST(1) << bits) - 1;

		if ((s->free[w] & want) != want)
			return false;
	}
	return true;
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
	const SiftFsmSlab *order[SIFT_FSM_MAX_SLABS];
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

static void
check_slab_room(Relation rel, Page root, int n)
{
	if (SLABS(root)->nslabs + n > SIFT_FSM_MAX_SLABS)
		ereport(ERROR, (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED), errmsg("sift index \"%s\" has the most metadata slabs (%zu)", RelationGetRelationName(rel), (size_t) SIFT_FSM_MAX_SLABS),
						errhint("REINDEX the index: a rebuild lays its metadata out afresh.")));
}

/* A new slab: a free extent of its class when there is one, else the end of the relation; never a split, which would need a page of a list */
void
sift_fsm_claim_slab(FsmCtx *c)
{
	SiftXlog	x;
	Buffer		rootbuf;
	Page		root;
	BlockNumber start;
	bool		have;
	uint32		nblocks;
	int			cls;

	rootbuf = sift_fsm_lock_root(c->rel);
	if (SLABS(BufferGetPage(rootbuf))->nslabs >= SIFT_FSM_MAX_SLABS && !c->in_reclaim)
	{
		UnlockReleaseBuffer(rootbuf);
		c->horizon = sift_horizon(c->rel);
		reclaim_before_claim(c);
		c->horizon = InvalidFullTransactionId;
		rootbuf = sift_fsm_lock_root(c->rel);
	}
	nblocks = SIFT_FSM_SLAB_BLOCKS(SLABS(BufferGetPage(rootbuf))->nslabs);
	cls = pg_ceil_log2_32(nblocks);
	have = FSMM(BufferGetPage(rootbuf))->class_free_extents[cls] > 0;
	UnlockReleaseBuffer(rootbuf);
	start = have ? sift_fsm_pop_class(c, cls).start : InvalidBlockNumber;
	x = sift_xlog_start(c->rel);
	rootbuf = sift_fsm_lock_root(c->rel);
	root = sift_xlog_register(&x, rootbuf, 0);
	check_slab_room(c->rel, root, 1);
	if (start == InvalidBlockNumber)
	{
		start = sift_fsm_high_water(c);
		sift_fsm_set_high_water(c, &x, start + nblocks);
	}
	sift_fsm_slab_add(root, start, nblocks);
	sift_fsm_root_seal(root);
	sift_xlog_finish(&x);
	UnlockReleaseBuffer(rootbuf);
}

/* Extents the caller took out of the lists (each at most SIFT_FSM_META_SLAB blocks) become metadata slabs, in the one record of the root */
void
sift_fsm_adopt_slabs(FsmCtx *c, const SiftExtent *ext, int n)
{
	SiftXlog	x = sift_xlog_start(c->rel);
	Buffer		rootbuf = sift_fsm_lock_root(c->rel);
	Page		root = sift_xlog_register(&x, rootbuf, 0);

	check_slab_room(c->rel, root, n);
	for (int i = 0; i < n; i++)
		sift_fsm_slab_add(root, ext[i].start, ext[i].nblocks);
	sift_fsm_root_seal(root);
	sift_xlog_done(&x);
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
		BlockNumber blk = sift_fsm_slab_take(root);

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

/* The last slab past the keep that is empty, or -1 */
static int
empty_slab(const Page root)
{
	const SiftFsmSlabTable *t = SLABS(root);

	for (int i = (int) t->nslabs - 1; i >= SIFT_FSM_KEEP_SLABS; i--)
		if (all_free(&t->slab[i]))
			return i;
	return -1;
}

/*
 * The slab leaves the root in one record and its blocks join the lists of the data zone in the next: a crash between the two leaves blocks that belong to no list and no part, which the orphan
 * sweep finds, never a block with two owners (the order of fsm.c for every step that moves an extent).
 */
uint32
sift_fsm_recycle_empty_slabs(FsmCtx *c)
{
	uint32		returned = 0;

	for (;;)
	{
		Buffer		rootbuf = sift_fsm_lock_root(c->rel);
		int			victim = empty_slab(BufferGetPage(rootbuf));
		SiftXlog	x;
		Page		root;
		SiftFsmSlabTable *t;
		SiftFsmExtent gone;

		if (victim < 0)
		{
			UnlockReleaseBuffer(rootbuf);
			return returned;
		}
		x = sift_xlog_start(c->rel);
		root = sift_xlog_register(&x, rootbuf, 0);
		t = SLABS(root);
		gone = (SiftFsmExtent) {.start = t->slab[victim].start,.nblocks = t->slab[victim].nblocks,.safexid = InvalidFullTransactionId};
		memmove(&t->slab[victim], &t->slab[victim + 1], (t->nslabs - victim - 1) * sizeof(SiftFsmSlab));
		memset(&t->slab[--t->nslabs], 0, sizeof(SiftFsmSlab));
		FSMM(root)->meta_free_count -= gone.nblocks;
		sift_fsm_root_seal(root);
		if (gone.start + gone.nblocks == sift_fsm_high_water(c))
		{
			sift_fsm_set_high_water(c, &x, gone.start);
			sift_xlog_finish(&x);
			UnlockReleaseBuffer(rootbuf);
		}
		else
		{
			sift_xlog_done(&x);
			sift_fsm_push_class(c, pg_ceil_log2_32(gone.nblocks), &gone, 1);
		}
		returned += gone.nblocks;
	}
}
