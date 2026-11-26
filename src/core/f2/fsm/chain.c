/*
 * core/f2/fsm/chain.c - the bitmaps of the metadata slabs of the allocator of format major 2 (core/f2/fsm/fsm.h), the root's and those of the chain of SIFT_PT_FSM_SLABS pages that holds the
 * slabs the root has no room for: a horizon that a long snapshot holds keeps the released metadata pages from maturing, and the slabs grow with the writes until it moves.
 *
 * A page of the chain is the first block of the first slab it holds, claimed with it in one record (block 0, the root, the page before it and the new page), and it leaves the chain with that slab
 * once it holds no other slab and the slab no other page in use. The pages of the chain change only under the exclusive lock of the root, which every writer of a bitmap holds, so a reader that holds
 * the root shared finds the chain the root names. A record registers a page of the chain the first time it changes one of its slabs: the steps that hold the root and the pages of a list
 * have room for one, and what does not fit is taken before the record that names it, or given back after the record that drops it (sift_fsm_slab_mark), so that a crash between the two leaves an
 * orphan for the sweep, never a block with two owners.
 */
#include "core/host.h"

#include "core/f2/fsm/int.h"
#include "core/page_io.h"

#define CHAINP(page)	((SiftFsmChainPageData *) SiftPageGetData(page))

static SiftFsmSlabTable *
table_of(const SiftFsmSlabDir *d, int p)
{
	return p == 0 ? SLABS(d->page[0]) : &CHAINP(d->page[p])->slabs;
}

static void
chain_seal(Page page)
{
	SiftPageSetUsed(page, offsetof(SiftFsmChainPageData, slabs.slab) + CHAINP(page)->slabs.nslabs * sizeof(SiftFsmSlab));
	sift_page_seal(page);
}

static void
check_chain_page(Relation rel, Page page, BlockNumber blk)
{
	const SiftFsmSlabTable *t = &CHAINP(page)->slabs;

	sift_page_expect(rel, page, blk, SIFT_PT_FSM_SLABS, 0, 0);
	if (CHAINP(page)->magic != SIFT_FSM_CHAIN_MAGIC || !sift_page_sum_ok(page) || t->nslabs == 0 || t->nslabs > SIFT_FSM_CHAIN_SLABS ||
		t->slab[0].start != blk || (t->slab[0].free[0] & 1) != 0)
		SIFT_CORRUPT(rel, "The page of metadata slabs at block %u is damaged.", blk);
}

static void
index_slabs(SiftFsmSlabDir *d)
{
	uint32		k = 0;

	d->n = 0;
	for (int p = 0; p < d->npages; p++)
		d->n += table_of(d, p)->nslabs;
	d->slab = repalloc(d->slab, Max(d->n, 1) * sizeof(SiftFsmSlab *));
	d->on = repalloc(d->on, Max(d->n, 1) * sizeof(int));
	for (int p = 0; p < d->npages; p++)
		for (uint32 i = 0; i < table_of(d, p)->nslabs; i++)
		{
			d->slab[k] = &table_of(d, p)->slab[i];
			d->on[k++] = p;
		}
}

static void
add_page(SiftFsmSlabDir *d, BlockNumber blk, Page page, bool own)
{
	if (d->npages == d->maxpages)
	{
		d->maxpages *= 2;
		d->blk = repalloc(d->blk, d->maxpages * sizeof(BlockNumber));
		d->page = repalloc(d->page, d->maxpages * sizeof(Page));
		d->own = repalloc(d->own, d->maxpages * sizeof(bool));
	}
	d->blk[d->npages] = blk;
	d->page[d->npages] = page;
	d->own[d->npages++] = own;
}

/* The working copy of the page when the record holds it already */
static Page
registered(const SiftXlog *x, BlockNumber blk)
{
	for (int i = 0; x != NULL && i < x->n; i++)
		if (BufferGetBlockNumber(x->buf[i]) == blk)
			return x->page[i];
	return NULL;
}

static Page
read_copy(Relation rel, BlockNumber blk)
{
	Buffer		buf = sift_blk_read(rel, blk, BUFFER_LOCK_SHARE);
	Page		copy = palloc(BLCKSZ);

	check_chain_page(rel, BufferGetPage(buf), blk);
	memcpy(copy, BufferGetPage(buf), BLCKSZ);
	UnlockReleaseBuffer(buf);
	return copy;
}

SiftFsmSlabDir
sift_fsm_dir_read(Relation rel, Page root, SiftXlog *x)
{
	SiftFsmSlabDir d = {.rel = rel,.x = x,.maxpages = 4};

	d.blk = palloc(d.maxpages * sizeof(BlockNumber));
	d.page = palloc(d.maxpages * sizeof(Page));
	d.own = palloc(d.maxpages * sizeof(bool));
	d.slab = palloc(sizeof(SiftFsmSlab *));
	d.on = palloc(sizeof(int));
	add_page(&d, SIFT_FSM_BLKNO, root, false);
	for (BlockNumber blk = SLABS(root)->chain; blk != 0; blk = table_of(&d, d.npages - 1)->chain)
	{
		Page		page = registered(x, blk);

		if ((BlockNumber) d.npages > RelationGetNumberOfBlocks(rel))
			SIFT_CORRUPT(rel, "The chain of metadata slabs loops.");
		add_page(&d, blk, page != NULL ? page : read_copy(rel, blk), page == NULL);
	}
	index_slabs(&d);
	return d;
}

void
sift_fsm_dir_free(SiftFsmSlabDir *d)
{
	for (int p = 0; p < d->npages; p++)
		if (d->own[p])
			pfree(d->page[p]);
	pfree(d->blk);
	pfree(d->page);
	pfree(d->own);
	pfree(d->slab);
	pfree(d->on);
}

/* The table of page p to change: a page of the chain is registered in the record the first time (a directory with no record changes its copies) */
static SiftFsmSlabTable *
writable(SiftFsmSlabDir *d, int p)
{
	if (d->x != NULL && d->own[p])
	{
		Buffer		buf = sift_blk_read(d->rel, d->blk[p], BUFFER_LOCK_EXCLUSIVE);

		check_chain_page(d->rel, BufferGetPage(buf), d->blk[p]);
		pfree(d->page[p]);
		d->page[p] = sift_xlog_register(d->x, buf, 0);
		d->own[p] = false;
		index_slabs(d);
	}
	return table_of(d, p);
}

static void
changed(SiftFsmSlabDir *d, int p)
{
	if (p > 0)
		chain_seal(d->page[p]);
}

int
sift_fsm_dir_find(const SiftFsmSlabDir *d, BlockNumber blk)
{
	for (uint32 k = 0; k < d->n; k++)
		if (blk >= d->slab[k]->start && blk < d->slab[k]->start + d->slab[k]->nblocks)
			return (int) k;
	return -1;
}

bool
sift_fsm_dir_fits(const SiftFsmSlabDir *d, BlockNumber blk)
{
	int			k = sift_fsm_dir_find(d, blk);

	return k < 0 || d->on[k] == 0 || !d->own[d->on[k]] || d->x->n < MAX_GENERIC_XLOG_PAGES;
}

/* The bit of blk in slab k set (free) or cleared, with the count of the root */
static void
flip(SiftFsmSlabDir *d, int k, BlockNumber blk, bool free)
{
	int			p = d->on[k];
	SiftFsmSlab *s;
	uint64		bit = UINT64CONST(1) << ((blk - d->slab[k]->start) % 64);
	uint64	   *w;

	writable(d, p);
	s = d->slab[k];
	w = &s->free[(blk - s->start) / 64];
	if (free && (*w & bit) != 0)
		SIFT_CORRUPT(d->rel, "Metadata block %u is freed twice.", blk);
	if (!free && (*w & bit) == 0)
		SIFT_CORRUPT(d->rel, "Metadata block %u is taken while in use.", blk);
	*w ^= bit;
	if (free)
		FSMM(d->page[0])->meta_free_count++;
	else
		FSMM(d->page[0])->meta_free_count--;
	changed(d, p);
}

static int
find_or_corrupt(const SiftFsmSlabDir *d, BlockNumber blk)
{
	int			k = sift_fsm_dir_find(d, blk);

	if (k < 0)
		SIFT_CORRUPT(d->rel, "Block %u is freed or taken as a metadata page and is in no metadata slab.", blk);
	return k;
}

void
sift_fsm_dir_put(SiftFsmSlabDir *d, BlockNumber blk)
{
	flip(d, find_or_corrupt(d, blk), blk, true);
}

void
sift_fsm_dir_claim(SiftFsmSlabDir *d, BlockNumber blk)
{
	flip(d, find_or_corrupt(d, blk), blk, false);
}

static bool
has_free(const SiftFsmSlab *s)
{
	for (int w = 0; w < SIFT_FSM_META_SLAB / 64; w++)
		if (s->free[w] != 0)
			return true;
	return false;
}

static int
lowest_with_free(const SiftFsmSlabDir *d)
{
	int			low = -1;

	for (uint32 k = 0; k < d->n; k++)
		if (has_free(d->slab[k]) && (low < 0 || d->slab[k]->start < d->slab[low]->start))
			low = (int) k;
	return low;
}

/* The lowest free block of the slabs, or InvalidBlockNumber when all are full: the metadata stays at the front of the file, so that the slabs behind it can empty */
BlockNumber
sift_fsm_dir_take(SiftFsmSlabDir *d)
{
	int			low = lowest_with_free(d);
	const SiftFsmSlab *s;

	if (low < 0)
		return InvalidBlockNumber;
	s = d->slab[low];
	for (int w = 0;; w++)
		if (s->free[w] != 0)
		{
			BlockNumber blk = s->start + w * 64 + pg_rightmost_one_pos64(s->free[w]);

			flip(d, low, blk, false);
			return blk;
		}
}

/* At most n free blocks of the lowest slab that has one, none past SIFT_FSM_POOL_SPAN blocks from its lowest free block: *lo is that block and each bit of *mask a block taken from the window; how many */
int
sift_fsm_dir_take_window(SiftFsmSlabDir *d, int n, BlockNumber *lo, uint32 *mask)
{
	int			low = lowest_with_free(d);
	SiftFsmSlab *s;
	int			got = 0;

	*mask = 0;
	if (low < 0)
		return 0;
	writable(d, d->on[low]);
	s = d->slab[low];
	for (uint32 b = 0; b < s->nblocks && got < n; b++)
	{
		uint64		bit = UINT64CONST(1) << (b % 64);

		if ((s->free[b / 64] & bit) == 0)
			continue;
		if (got == 0)
			*lo = s->start + b;
		if (s->start + b - *lo >= SIFT_FSM_POOL_SPAN)
			break;
		s->free[b / 64] &= ~bit;
		*mask |= UINT32_C(1) << (s->start + b - *lo);
		got++;
	}
	FSMM(d->page[0])->meta_free_count -= got;
	changed(d, d->on[low]);
	return got;
}

/* A new page of the chain at the first block of the new slab, which it holds, linked behind the last page */
static void
chain_page_new(SiftFsmSlabDir *d, FsmCtx *c, BlockNumber start, uint32 nblocks)
{
	int			last = d->npages - 1;
	Page		page = sift_xlog_register(d->x, sift_blk_new(d->rel, start), GENERIC_XLOG_FULL_IMAGE);
	SiftFsmChainPageData *cp;

	sift_page_init_part(page, SIFT_PT_FSM_SLABS, 0, 0);
	cp = CHAINP(page);
	cp->magic = SIFT_FSM_CHAIN_MAGIC;
	cp->slabs.nslabs = 1;
	sift_fsm_slab_init(&cp->slabs.slab[0], start, nblocks);
	cp->slabs.slab[0].free[0] &= ~UINT64CONST(1);
	chain_seal(page);
	writable(d, last)->chain = start;
	changed(d, last);
	if (last == 0)
		sift_fsm_set_chain_flag(c, d->x, true);
	FSMM(d->page[0])->meta_free_count += nblocks - 1;
	add_page(d, start, page, false);
}

/* A new slab: in the root while it has room, else in the first page of the chain that has, else in a new page of the chain */
void
sift_fsm_dir_add(SiftFsmSlabDir *d, FsmCtx *c, BlockNumber start, uint32 nblocks)
{
	int			p = 1;

	if (SLABS(d->page[0])->nslabs < SIFT_FSM_ROOT_SLABS)
		sift_fsm_slab_add(d->page[0], start, nblocks);
	else
	{
		while (p < d->npages && table_of(d, p)->nslabs >= SIFT_FSM_CHAIN_SLABS)
			p++;
		if (p == d->npages)
			chain_page_new(d, c, start, nblocks);
		else
		{
			sift_fsm_slab_insert(writable(d, p), start, nblocks);
			FSMM(d->page[0])->meta_free_count += nblocks;
			changed(d, p);
		}
	}
	index_slabs(d);
}

BlockNumber
sift_fsm_slab_take(Relation rel, SiftXlog *x, Page root)
{
	SiftFsmSlabDir d = sift_fsm_dir_read(rel, root, x);
	BlockNumber blk = sift_fsm_dir_take(&d);

	sift_fsm_dir_free(&d);
	return blk;
}

void
sift_fsm_slab_put(Relation rel, SiftXlog *x, Page root, BlockNumber blk)
{
	SiftFsmSlabDir d = sift_fsm_dir_read(rel, root, x);

	sift_fsm_dir_put(&d, blk);
	sift_fsm_dir_free(&d);
}

void
sift_fsm_slab_mark(Relation rel, const BlockNumber *blk, int n, bool free)
{
	while (n > 0)
	{
		SiftXlog	x = sift_xlog_start(rel);
		Page		root = sift_xlog_register(&x, sift_fsm_lock_root(rel), 0);
		SiftFsmSlabDir d = sift_fsm_dir_read(rel, root, &x);
		int			done = 0;

		for (; done < n && sift_fsm_dir_fits(&d, blk[done]); done++)
			flip(&d, find_or_corrupt(&d, blk[done]), blk[done], free);
		sift_fsm_dir_free(&d);
		sift_fsm_root_seal(root);
		sift_xlog_done(&x);
		blk += done;
		n -= done;
	}
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

/* The slab is the only one of its page of the chain and holds no page but that one */
static bool
lone_host(const SiftFsmSlabDir *d, int k)
{
	return d->on[k] > 0 && table_of(d, d->on[k])->nslabs == 1 && sift_fsm_slab_used(d->slab[k]) == 1;
}

/* The slab past the keep that goes back first: the last one of the chain, then of the root, that is empty, or -1 */
static int
empty_slab(const SiftFsmSlabDir *d)
{
	for (int k = (int) d->n - 1; k >= SIFT_FSM_KEEP_SLABS; k--)
		if (all_free(d->slab[k]) || lone_host(d, k))
			return k;
	return -1;
}

/* Slab k leaves its page, or the chain loses the page it is the lone slab of */
static void
drop_slab(SiftFsmSlabDir *d, FsmCtx *c, int k)
{
	int			p = d->on[k];
	SiftFsmMetaPageData *m = FSMM(d->page[0]);

	if (lone_host(d, k))
	{
		BlockNumber next = table_of(d, p)->chain;

		m->meta_free_count -= d->slab[k]->nblocks - 1;
		writable(d, p - 1)->chain = next;
		changed(d, p - 1);
		if (SLABS(d->page[0])->chain == 0)
			sift_fsm_set_chain_flag(c, d->x, false);
	}
	else
	{
		SiftFsmSlabTable *t = writable(d, p);
		uint32		at = d->slab[k] - t->slab;

		m->meta_free_count -= t->slab[at].nblocks;
		memmove(&t->slab[at], &t->slab[at + 1], (t->nslabs - at - 1) * sizeof(SiftFsmSlab));
		memset(&t->slab[--t->nslabs], 0, sizeof(SiftFsmSlab));
		changed(d, p);
	}
}

/*
 * The slab leaves the directory in one record and its blocks join the lists of the data zone in the next: a crash between the two leaves blocks that belong to no list and no part, which the orphan
 * sweep finds, never a block with two owners (the order of fsm.c for every step that moves an extent).
 */
uint32
sift_fsm_recycle_empty_slabs(FsmCtx *c)
{
	uint32		returned = 0;

	for (;;)
	{
		SiftXlog	x = sift_xlog_start(c->rel);
		Buffer		rootbuf = sift_fsm_lock_root(c->rel);
		Page		root = sift_xlog_register(&x, rootbuf, 0);
		SiftFsmSlabDir d = sift_fsm_dir_read(c->rel, root, &x);
		int			victim = empty_slab(&d);
		SiftFsmExtent gone;
		bool		at_end;

		if (victim < 0)
		{
			sift_fsm_dir_free(&d);
			sift_xlog_abort(&x);
			UnlockReleaseBuffer(rootbuf);
			return returned;
		}
		gone = (SiftFsmExtent) {.start = d.slab[victim]->start,.nblocks = d.slab[victim]->nblocks,.safexid = InvalidFullTransactionId};
		drop_slab(&d, c, victim);
		sift_fsm_dir_free(&d);
		sift_fsm_root_seal(root);
		at_end = gone.start + gone.nblocks == sift_fsm_high_water(c);
		if (at_end)
			sift_fsm_set_high_water(c, &x, gone.start);
		sift_fsm_xlog_done(c, &x);
		if (!at_end)
			sift_fsm_push_class(c, pg_ceil_log2_32(gone.nblocks), &gone, 1);
		returned += gone.nblocks;
	}
}
