/*
 * core/fsm.c - the allocator of format major 2 (core/fsm.h).
 *
 * Every public function takes the lock of block 0 first and keeps it to the end (the order of the locks is block 0, block 1, a page of a list): exclusively, so that the
 * lists only ever have one writer, except for an allocation from the lists, which holds it shared beside other allocations and does each of its steps in one record under block 1, taking
 * block 0 exclusively (after letting the shared hold go) only to move the high-water mark, reclaim or claim a slab; a record of generic WAL here registers at most block 0, block 1 and two pages of a list. A step that moves an extent between lists is one
 * record per list: a crash between two of them leaves blocks that belong to no list and no part, which the orphan sweep of the index finds, never a block with two owners.
 */
#include "core/host.h"

#include "catalog/storage.h"
#include "catalog/storage_xlog.h"
#include "miscadmin.h"
#include "postmaster/bgworker.h"
#include "storage/latch.h"
#include "storage/lmgr.h"
#include "storage/proc.h"
#include "storage/smgr.h"
#include "utils/timestamp.h"
#include "utils/wait_event.h"

#include "col/f2/meta.h"
#include "core/f2/fsm/fsm.h"
#include "core/f2/pace.h"
#include "core/f2/fsm/int.h"
#include "core/horizon.h"
#include "core/maint/maint.h"
#include "core/page_io.h"
#include "core/restrict.h"
#include "core/shm/lease.h"
#include "core/wal_conflict.h"

void
sift_fsm_extvec_add(ExtVec *e, BlockNumber start, uint32 nblocks, FullTransactionId safexid)
{
	if (e->n == e->max)
	{
		e->max = Max(64, e->max * 2);
		e->v = e->v ? repalloc(e->v, e->max * sizeof(SiftFsmExtent)) : palloc(e->max * sizeof(SiftFsmExtent));
	}
	e->v[e->n++] = (SiftFsmExtent) {.start = start,.nblocks = nblocks,.safexid = safexid};
}

int
sift_fsm_ext_cmp(const void *a, const void *b)
{
	BlockNumber x = ((const SiftFsmExtent *) a)->start,
				y = ((const SiftFsmExtent *) b)->start;

	return x < y ? -1 : x > y;
}

void
sift_fsm_root_seal(Page page)
{
	SiftPageSetUsed(page, SIFT_FSM_SLABS_OFF + offsetof(SiftFsmSlabTable, slab) + SLABS(page)->nslabs * sizeof(SiftFsmSlab));
	sift_page_seal(page);
}

void
sift_fsm_list_seal(Page page)
{
	SiftPageSetUsed(page, offsetof(SiftFsmPageData, ext) + FSMP(page)->nentries * sizeof(SiftFsmExtent));
	sift_page_seal(page);
}

/* The smallest class that holds nblocks */
static int
class_ceil(uint32 nblocks)
{
	for (int c = 0; c < SIFT_FSM_NCLASSES; c++)
		if (nblocks <= SIFT_FSM_CLASS_BLOCKS(c))
			return c;
	ereport(ERROR, (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED), errmsg("sift: an extent of %u blocks is more than the %u of the largest size class", nblocks, SIFT_FSM_MAX_EXTENT),
					errhint("Ask for several extents (sift_fsm_alloc_stream).")));
	pg_unreachable();
}

/* The largest class that fits in nblocks (at least 1) */
int
sift_fsm_class_floor(uint64 nblocks)
{
	int			c = SIFT_FSM_NCLASSES - 1;

	while (SIFT_FSM_CLASS_BLOCKS(c) > nblocks)
		c--;
	return c;
}

void
sift_fsm_build_root(Page page, BlockNumber slab_start)
{
	SiftFsmMetaPageData *m;
	SiftFsmSlab *slab;

	sift_page_init_part(page, SIFT_PT_FSM_META, 0, 0);
	m = FSMM(page);
	m->magic = SIFT_FSM_MAGIC;
	m->version = SIFT_FSM_VERSION;
	for (int c = 0; c < SIFT_FSM_NCLASSES; c++)
		m->class_head[c] = InvalidBlockNumber;
	m->quarantine_head = m->quarantine_tail = InvalidBlockNumber;
	m->meta_free_count = SIFT_FSM_SLAB_FIRST - 1;
	SLABS(page)->nslabs = 1;
	slab = &SLABS(page)->slab[0];
	sift_fsm_slab_init(slab, slab_start, SIFT_FSM_SLAB_FIRST);
	slab->free[0] &= ~UINT64CONST(1);
	sift_fsm_root_seal(page);
}

void
sift_fsm_check_root(Relation rel, Buffer buf)
{
	Page		page = BufferGetPage(buf);

	sift_page_expect(rel, page, BufferGetBlockNumber(buf), SIFT_PT_FSM_META, 0, 0);
	if (FSMM(page)->magic != SIFT_FSM_MAGIC || !sift_page_sum_ok(page) || SLABS(page)->nslabs > SIFT_FSM_ROOT_SLABS)
		SIFT_CORRUPT(rel, "The allocator root (block %u) is damaged.", BufferGetBlockNumber(buf));
}

Buffer
sift_fsm_lock_root(Relation rel)
{
	Buffer		buf = sift_blk_read(rel, SIFT_FSM_BLKNO, BUFFER_LOCK_EXCLUSIVE);

	sift_fsm_check_root(rel, buf);
	return buf;
}

Buffer
sift_fsm_lock_list_page(Relation rel, BlockNumber blk, uint16 size_class)
{
	Buffer		buf = sift_blk_read(rel, blk, BUFFER_LOCK_EXCLUSIVE);
	Page		page = BufferGetPage(buf);

	sift_page_expect(rel, page, blk, SIFT_PT_FSM_PAGE, 0, 0);
	if (FSMP(page)->magic != SIFT_FSM_MAGIC || FSMP(page)->size_class != size_class || !sift_page_sum_ok(page) || FSMP(page)->nentries > SIFT_FSM_PAGE_CAP)
		SIFT_CORRUPT(rel, "The allocator page at block %u is damaged.", blk);
	return buf;
}

void
sift_fsm_init_list_page(Page page, uint16 size_class, uint16 flags, BlockNumber next)
{
	sift_page_init_part(page, SIFT_PT_FSM_PAGE, 0, 0);
	FSMP(page)->magic = SIFT_FSM_MAGIC;
	FSMP(page)->flags = flags;
	FSMP(page)->size_class = size_class;
	FSMP(page)->next_blk = next;
}

void
sift_fsm_set_high_water(FsmCtx *c, SiftXlog *x, BlockNumber high_water)
{
	Page		p0;

	Assert(!c->shared);
	p0 = sift_xlog_register(x, c->buf0, 0);
	SiftColMetaPageData *m = SIFT_COL_META(p0);

	m->high_water = high_water;
	sift_meta_seal(p0, m->common.struct_size);
}

void
sift_fsm_set_chain_flag(FsmCtx *c, SiftXlog *x, bool on)
{
	Page		p0;
	SiftColMetaPageData *m;

	Assert(!c->shared);
	p0 = sift_xlog_register(x, c->buf0, 0);
	m = SIFT_COL_META(p0);
	m->common.ro_compat_flags = on ? m->common.ro_compat_flags | SIFT_ROCOMPAT_FSM_CHAIN : m->common.ro_compat_flags & ~SIFT_ROCOMPAT_FSM_CHAIN;
	sift_meta_seal(p0, m->common.struct_size);
}

/* Logs the record and lets go of every page it holds but block 0, which the call keeps */
void
sift_fsm_xlog_done(FsmCtx *c, SiftXlog *x)
{
	sift_xlog_finish(x);
	for (int i = 0; i < x->n; i++)
		if (x->buf[i] != c->buf0)
			UnlockReleaseBuffer(x->buf[i]);
}

BlockNumber
sift_fsm_high_water(FsmCtx *c)
{
	return SIFT_COL_META(BufferGetPage(c->buf0))->high_water;
}

/* Moves high_water over n blocks, in a record of its own (P3): the first block of them */
static BlockNumber
extend_high_water(FsmCtx *c, uint32 n)
{
	SiftXlog	x;
	BlockNumber start;

	sift_fsm_ctx_exclusive(c);
	x = sift_xlog_start(c->rel);
	start = sift_fsm_high_water(c);

	if ((uint64) start + n >= SIFT_F2_LOCK_FLOOR)
		ereport(ERROR, (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED), errmsg("sift index \"%s\" cannot grow past %u blocks", RelationGetRelationName(c->rel), SIFT_F2_LOCK_FLOOR),
						errhint("Split the table, or partition it, so that each index is smaller.")));
	sift_fsm_set_high_water(c, &x, start + n);
	sift_xlog_finish(&x);
	return start;
}

/*
 * Takes the last entry of the head page of the list of class cls into *e, in the record x that holds the root: a head page it empties is unlinked, and its block is *emptied (else InvalidBlockNumber) for
 * the caller to give back. False when the list is empty, which an allocation that holds block 0 shared finds when another one took the last extent since it looked
 */
bool
sift_fsm_pop_class_in(FsmCtx *c, SiftXlog *x, Page root, int cls, SiftFsmExtent *e, BlockNumber *emptied)
{
	SiftFsmMetaPageData *m = FSMM(root);
	BlockNumber head = m->class_head[cls];
	Page		hp;

	*emptied = InvalidBlockNumber;
	if (head == InvalidBlockNumber && m->class_free_extents[cls] == 0)
		return false;
	if (head == InvalidBlockNumber || m->class_free_extents[cls] == 0)
		SIFT_CORRUPT(c->rel, "The allocator has no free extent of class %d.", cls);
	hp = sift_xlog_register(x, sift_fsm_lock_list_page(c->rel, head, cls), 0);
	*e = FSMP(hp)->ext[--FSMP(hp)->nentries];
	memset(&FSMP(hp)->ext[FSMP(hp)->nentries], 0, sizeof(SiftFsmExtent));
	if (FSMP(hp)->nentries == 0)
	{
		m->class_head[cls] = FSMP(hp)->next_blk;
		*emptied = head;
	}
	m->class_free_extents[cls]--;
	sift_fsm_list_seal(hp);
	return true;
}

bool
sift_fsm_pop_class(FsmCtx *c, int cls, SiftFsmExtent *e)
{
	SiftXlog	x = sift_xlog_start(c->rel);
	Buffer		rootbuf = sift_fsm_lock_root(c->rel);
	Page		root = sift_xlog_register(&x, rootbuf, 0);
	BlockNumber emptied;

	if (!sift_fsm_pop_class_in(c, &x, root, cls, e, &emptied))
	{
		sift_xlog_abort(&x);
		UnlockReleaseBuffer(rootbuf);
		return false;
	}
	if (emptied != InvalidBlockNumber)
		sift_fsm_slab_put(c->rel, &x, root, emptied);
	sift_fsm_root_seal(root);
	sift_xlog_done(&x);
	return true;
}

/* Puts n extents of the class on its list: onto the head page while it has room, else onto a new head page from the metadata slabs */
void
sift_fsm_push_class(FsmCtx *c, int cls, const SiftFsmExtent *ext, int n)
{
	while (n > 0)
	{
		SiftXlog	x = sift_xlog_start(c->rel);
		Buffer		rootbuf = sift_fsm_lock_root(c->rel);
		Page		root = sift_xlog_register(&x, rootbuf, 0);
		SiftFsmMetaPageData *m = FSMM(root);
		BlockNumber head = m->class_head[cls];
		Page		page;
		int			room;
		int			take;

		if (head != InvalidBlockNumber)
		{
			Buffer		hbuf = sift_fsm_lock_list_page(c->rel, head, cls);

			page = sift_xlog_register(&x, hbuf, 0);
			room = SIFT_FSM_PAGE_CAP - FSMP(page)->nentries;
			if (room == 0)
				page = NULL;
		}
		else
			page = NULL;
		if (page == NULL)
		{
			BlockNumber blk = sift_fsm_slab_take(c->rel, &x, root);
			Buffer		nbuf;

			if (blk == InvalidBlockNumber)
			{
				sift_xlog_abort(&x);
				for (int i = 0; i < x.n; i++)
					UnlockReleaseBuffer(x.buf[i]);
				sift_fsm_grow_meta(c);
				continue;
			}
			nbuf = sift_blk_new(c->rel, blk);
			page = sift_xlog_register(&x, nbuf, GENERIC_XLOG_FULL_IMAGE);
			sift_fsm_init_list_page(page, cls, 0, head);
			m->class_head[cls] = blk;
			room = SIFT_FSM_PAGE_CAP;
		}
		take = Min(room, n);
		if (take == 1 && FSMP(page)->nentries > 0)
		{
			/* the page is popped from its end: one extent goes in so that the lowest block stays last */
			uint32		at = 0;

			while (at < FSMP(page)->nentries && FSMP(page)->ext[at].start > ext->start)
				at++;
			memmove(&FSMP(page)->ext[at + 1], &FSMP(page)->ext[at], (FSMP(page)->nentries - at) * sizeof(SiftFsmExtent));
			FSMP(page)->ext[at] = *ext;
		}
		else
			memcpy(&FSMP(page)->ext[FSMP(page)->nentries], ext, take * sizeof(SiftFsmExtent));
		FSMP(page)->nentries += take;
		m->class_free_extents[cls] += take;
		sift_fsm_list_seal(page);
		sift_fsm_root_seal(root);
		sift_xlog_done(&x);
		ext += take;
		n -= take;
	}
}

/* The block of the lowest free extent of class cls (the head page of its list is popped from its end), false when there is none; the caller holds the root */
static bool
peek_lowest(FsmCtx *c, const SiftFsmMetaPageData *m, int cls, BlockNumber *start)
{
	Buffer		hbuf;
	Page		hp;

	if (m->class_head[cls] == InvalidBlockNumber || m->class_free_extents[cls] == 0)
		return false;
	hbuf = sift_fsm_lock_list_page(c->rel, m->class_head[cls], cls);
	hp = BufferGetPage(hbuf);
	*start = FSMP(hp)->ext[FSMP(hp)->nentries - 1].start;
	UnlockReleaseBuffer(hbuf);
	return true;
}

/* The class, from `cls` up, of the free extent that starts lowest, in one look at the root; SIFT_FSM_NCLASSES when there is none */
static int
lowest_class(FsmCtx *c, int cls)
{
	Buffer		rootbuf = sift_fsm_lock_root(c->rel);
	const SiftFsmMetaPageData *m = FSMM(BufferGetPage(rootbuf));
	int			found = SIFT_FSM_NCLASSES;
	BlockNumber lowest = InvalidBlockNumber;

	for (int k = cls; k < SIFT_FSM_NCLASSES; k++)
	{
		BlockNumber start;

		if (peek_lowest(c, m, k, &start) && (found == SIFT_FSM_NCLASSES || start < lowest))
		{
			found = k;
			lowest = start;
		}
	}
	UnlockReleaseBuffer(rootbuf);
	return found;
}

/* The class, from `cls` up, of the free extent that starts lowest, reclaiming the queue once when there is none; SIFT_FSM_NCLASSES when the file must grow. Allocating at the lowest address keeps the end of the file free to be cut */
static int
find_class(FsmCtx *c, int cls)
{
	for (int round = 0;; round++)
	{
		Buffer		rootbuf = sift_fsm_lock_root(c->rel);
		bool		queued = FSMM(BufferGetPage(rootbuf))->quarantine_extents > 0;
		int			found;

		UnlockReleaseBuffer(rootbuf);
		found = lowest_class(c, cls);
		if (found < SIFT_FSM_NCLASSES || !queued || round == 1)
			return found;
		sift_fsm_reclaim_locked(c, NULL);
	}
}

/* The first n blocks of a free extent of class k; the rest of it goes back to the lists, cut into the largest classes that fit. InvalidBlockNumber when the class has none left */
static BlockNumber
carve_free(FsmCtx *c, int k, uint32 n)
{
	SiftFsmExtent e;
	uint64		at,
				left = SIFT_FSM_CLASS_BLOCKS(k) - n;

	if (!sift_fsm_pop_class(c, k, &e))
		return InvalidBlockNumber;
	at = (uint64) e.start + n;

	while (left > 0)
	{
		int			j = sift_fsm_class_floor(left);
		SiftFsmExtent piece = {.start = (BlockNumber) at,.nblocks = SIFT_FSM_CLASS_BLOCKS(j),.safexid = InvalidFullTransactionId};

		sift_fsm_push_class(c, j, &piece, 1);
		at += piece.nblocks;
		left -= piece.nblocks;
	}
	return e.start;
}

/* n blocks in one extent: off a free extent of class k (SIFT_FSM_NCLASSES: there is none that holds them) or the end of the relation; false when another allocation took the extents of class k first */
static bool
take_extent(FsmCtx *c, int k, uint32 n, SiftExtent *out)
{
	out->nblocks = n;
	out->start = k == SIFT_FSM_NCLASSES ? extend_high_water(c, n) : carve_free(c, k, n);
	if (out->start == InvalidBlockNumber)
		return false;
	if (sift_fsm_below_reserve(c))
		sift_fsm_grow_meta(c);
	return true;
}

static SiftExtent
alloc_locked(FsmCtx *c, uint32 nblocks)
{
	SiftExtent	e;

	while (!take_extent(c, find_class(c, class_ceil(nblocks)), nblocks, &e))
		;
	return e;
}

FsmCtx
sift_fsm_ctx_open(Relation rel)
{
	SIFT_NOTE_BLOCK0_X(SIFT_B0_FSM);
	return (FsmCtx) {.rel = rel,.buf0 = sift_col_meta_lock(rel, BUFFER_LOCK_EXCLUSIVE)};
}

/*
 * An allocation from the lists holds block 0 shared: it keeps out the steps that plan their changes of the lists in more than one look at the root (reclaim, the sweep, the rehome, a commit that
 * releases inside its record) and lets other allocations and the readers of the metapage in; each of its steps is one record under the root. It takes block 0 exclusively for the rest of the
 * call only where it moves the high-water mark, reclaims the queue or claims a slab
 */
static FsmCtx
ctx_open_shared(Relation rel)
{
	return (FsmCtx) {.rel = rel,.buf0 = sift_col_meta_lock(rel, BUFFER_LOCK_SHARE),.shared = true};
}

void
sift_fsm_ctx_exclusive(FsmCtx *c)
{
	if (!c->shared)
		return;
	UnlockReleaseBuffer(c->buf0);
	c->shared = false;
	c->buf0 = sift_fsm_ctx_open(c->rel).buf0;
}

void
sift_fsm_ctx_close(FsmCtx *c)
{
	UnlockReleaseBuffer(c->buf0);
}

SiftExtent
sift_fsm_alloc(Relation rel, uint32 nblocks)
{
	FsmCtx		c = ctx_open_shared(rel);
	SiftExtent	e = alloc_locked(&c, nblocks);

	sift_fsm_ctx_close(&c);
	return e;
}

/* The lowest class of at most the n blocks still wanted whose free extent starts (with that many blocks) below max_blk: the class to carve and the class whose list it comes from, false when none does */
static bool
find_below(FsmCtx *c, uint64 npages, BlockNumber max_blk, int *cls, int *from)
{
	Buffer		rootbuf = sift_fsm_lock_root(c->rel);
	const SiftFsmMetaPageData *m = FSMM(BufferGetPage(rootbuf));
	bool		found = false;

	for (int j = sift_fsm_class_floor((uint32) Min(npages, SIFT_FSM_MAX_EXTENT)); j >= 0 && !found; j--)
		for (int k = j; k < SIFT_FSM_NCLASSES && !found; k++)
		{
			BlockNumber start;

			if (peek_lowest(c, m, k, &start) && (uint64) start + SIFT_FSM_CLASS_BLOCKS(j) <= max_blk)
			{
				*cls = j;
				*from = k;
				found = true;
			}
		}
	UnlockReleaseBuffer(rootbuf);
	return found;
}

int
sift_fsm_alloc_below(Relation rel, uint64 npages, BlockNumber max_blk, SiftExtent *out, int max)
{
	FsmCtx		c = sift_fsm_ctx_open(rel);
	int			n = 0;

	sift_fsm_reclaim_locked(&c, NULL);
	while (npages > 0)
	{
		int			cls,
					from;

		if (n == max || !find_below(&c, npages, max_blk, &cls, &from))
		{
			for (int i = 0; i < n; i++)
			{
				SiftFsmExtent back = {.start = out[i].start,.nblocks = out[i].nblocks,.safexid = InvalidFullTransactionId};

				sift_fsm_push_class(&c, sift_fsm_class_floor(out[i].nblocks), &back, 1);
			}
			n = 0;
			break;
		}
		out[n] = (SiftExtent) {.start = carve_free(&c, from, SIFT_FSM_CLASS_BLOCKS(cls)),.nblocks = SIFT_FSM_CLASS_BLOCKS(cls)};
		npages -= out[n].nblocks;
		n++;
	}
	sift_fsm_ctx_close(&c);
	return n;
}

/* The class of the smaller free extent a stream takes before it extends the file, when none of the class it wants is free: the largest that leaves at most SIFT_FSM_REUSE_PIECES extents of it (SIFT_FSM_SMALL_PIECES below SIFT_FSM_SMALL_CLASS) for the n blocks still wanted and fits the room of the caller; the class of the free extent it is carved from in *from, -1 when there is none */
static int
reuse_class(FsmCtx *c, uint64 nblocks, int cls, int room, int *from)
{
	for (int j = cls - 1; j >= 0; j--)
	{
		uint64		pieces = (nblocks + SIFT_FSM_CLASS_BLOCKS(j) - 1) / SIFT_FSM_CLASS_BLOCKS(j);

		if (pieces > (j < SIFT_FSM_SMALL_CLASS ? SIFT_FSM_SMALL_PIECES : SIFT_FSM_REUSE_PIECES))
			break;
		if (pieces <= (uint64) room && (*from = lowest_class(c, j)) < SIFT_FSM_NCLASSES)
			return j;
	}
	return -1;
}

/* STOR[STOR-F2-03]: FSM small-index meta reserve and multi-extent fallback (cuts EOF growth) */
int
sift_fsm_alloc_stream(Relation rel, uint64 nblocks, SiftExtent *out, int max)
{
	FsmCtx		c = ctx_open_shared(rel);
	int			n = 0;

	while (nblocks > 0)
	{
		uint32		top = (uint32) Min(nblocks, SIFT_FSM_MAX_EXTENT);
		int			cls = sift_fsm_class_floor(top),
					k = find_class(&c, cls),
					small = -1;

		if (n == max)
			ereport(ERROR, (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED), errmsg("sift: a stream of more than %d extents", max),
							errhint("Write the stream in several parts.")));
		if (k == SIFT_FSM_NCLASSES && (small = reuse_class(&c, nblocks, cls, max - n, &k)) >= 0)
			cls = small;
		if (!take_extent(&c, k, k == SIFT_FSM_NCLASSES ? top : SIFT_FSM_CLASS_BLOCKS(cls), &out[n]))
			continue;
		nblocks -= out[n].nblocks;
		n++;
	}
	sift_fsm_ctx_close(&c);
	return n;
}

#define META_POOL_RETRY_EVERY	8

static bool meta_batch_dry;
static int	meta_batch_dry_misses;

/*
 * The empty pool of the file takes the pages of one batch; what it keeps no room for goes back to the slabs. A batch that the slabs cannot give (they hold no more than their reserve) is not asked for
 * again by this backend until it has grown a slab itself, or has gone META_POOL_RETRY_EVERY pages without
 */
static bool
meta_pool_refill(Relation rel)
{
	BlockNumber pages[SIFT_LEASE_PAGES];
	bool		pool_lost;
	int			n = 0,
				kept;

	StaticAssertStmt(SIFT_LEASE_PAGES == SIFT_FSM_POOL_SPAN, "a pool is one window of the root");
	if (meta_batch_dry && ++meta_batch_dry_misses % META_POOL_RETRY_EVERY != 0)
		return false;
	if (!sift_lease_page_room(&rel->rd_locator, &pool_lost))
		return false;
	PG_TRY();
	{
		n = sift_fsm_alloc_meta_batch(rel, pages, pool_lost);
	}
	PG_CATCH();
	{
		(void) sift_lease_page_fill(NULL, 0);
		PG_RE_THROW();
	}
	PG_END_TRY();
	kept = sift_lease_page_fill(pages, n);
	sift_fsm_free_meta_batch(rel, pages + kept, n - kept);
	meta_batch_dry = kept == 0;
	return kept > 0;
}

void
sift_fsm_meta_pool_drain(Relation rel)
{
	BlockNumber pages[SIFT_LEASE_PAGES];
	int			n = sift_lease_page_drain(&rel->rd_locator, pages);

	sift_fsm_free_meta_batch(rel, pages, n);
}

BlockNumber
sift_fsm_alloc_meta_page(Relation rel)
{
	BlockNumber blk;
	FsmCtx		c;
	bool		grew;

	if (sift_lease_page_pop(&rel->rd_locator, &blk) || (meta_pool_refill(rel) && sift_lease_page_pop(&rel->rd_locator, &blk)))
		return blk;
	c = sift_fsm_ctx_open(rel);
	blk = sift_fsm_alloc_meta_locked(&c);
	grew = sift_fsm_below_reserve(&c);
	if (grew)
		sift_fsm_grow_meta(&c);
	sift_fsm_ctx_close(&c);
	if (grew)
	{
		meta_batch_dry = false;
		(void) meta_pool_refill(rel);
	}
	return blk;
}

/* Is there room for n entries in the tail of the queue; the tail page locked and checked when it is (or InvalidBuffer) */
static Buffer
queue_tail_with_room(Relation rel, Page root, int n)
{
	BlockNumber tail = FSMM(root)->quarantine_tail;
	Buffer		buf;

	if (tail == InvalidBlockNumber)
		return InvalidBuffer;
	buf = sift_fsm_lock_list_page(rel, tail, SIFT_FSM_CLASS_QUEUE);
	if (SIFT_FSM_PAGE_CAP - FSMP(BufferGetPage(buf))->nentries >= (uint32) n)
		return buf;
	UnlockReleaseBuffer(buf);
	return InvalidBuffer;
}

static void
queue_new_tail(FsmCtx *c)
{
	for (;;)
	{
		SiftXlog	x = sift_xlog_start(c->rel);
		Buffer		rootbuf = sift_fsm_lock_root(c->rel);
		Page		root = sift_xlog_register(&x, rootbuf, 0);
		SiftFsmMetaPageData *m = FSMM(root);
		BlockNumber blk = sift_fsm_slab_take(c->rel, &x, root);
		Page		page;

		if (blk == InvalidBlockNumber)
		{
			sift_xlog_abort(&x);
			UnlockReleaseBuffer(rootbuf);
			sift_fsm_grow_meta(c);
			continue;
		}
		if (m->quarantine_tail != InvalidBlockNumber)
		{
			Buffer		obuf = sift_fsm_lock_list_page(c->rel, m->quarantine_tail, SIFT_FSM_CLASS_QUEUE);
			Page		old = sift_xlog_register(&x, obuf, 0);

			FSMP(old)->next_blk = blk;
			sift_fsm_list_seal(old);
		}
		else
			m->quarantine_head = blk;
		m->quarantine_tail = blk;
		page = sift_xlog_register(&x, sift_blk_new(c->rel, blk), GENERIC_XLOG_FULL_IMAGE);
		sift_fsm_init_list_page(page, SIFT_FSM_CLASS_QUEUE, SIFT_FSM_PAGE_DEFERRED, InvalidBlockNumber);
		sift_fsm_list_seal(page);
		sift_fsm_root_seal(root);
		sift_xlog_done(&x);
		return;
	}
}

void
sift_fsm_reserve_release(Relation rel, Buffer buf0, int n)
{
	FsmCtx		c = {.rel = rel,.buf0 = buf0};
	Buffer		rootbuf = sift_fsm_lock_root(rel);
	Buffer		tail = queue_tail_with_room(rel, BufferGetPage(rootbuf), n);

	UnlockReleaseBuffer(rootbuf);
	if (tail != InvalidBuffer)
	{
		UnlockReleaseBuffer(tail);
		return;
	}
	queue_new_tail(&c);
}

void
sift_fsm_release_in(Relation rel, SiftXlog *x, const SiftExtent *ext, int n, FullTransactionId safexid)
{
	Buffer		rootbuf = sift_fsm_lock_root(rel);
	Page		root = sift_xlog_register(x, rootbuf, 0);
	SiftFsmMetaPageData *m = FSMM(root);
	Buffer		tail;
	Page		page;

	sift_fsm_release_guard(rel, root, ext, n);
	tail = queue_tail_with_room(rel, root, n);
	if (tail == InvalidBuffer)
		SIFT_CORRUPT(rel, "The queue of released extents has no room for %d: sift_fsm_reserve_release was not called.", n);
	page = sift_xlog_register(x, tail, 0);
	for (int i = 0; i < n; i++)
	{
		FSMP(page)->ext[FSMP(page)->nentries++] = (SiftFsmExtent) {.start = ext[i].start,.nblocks = ext[i].nblocks,.safexid = safexid};
		m->quarantine_blocks += ext[i].nblocks;
	}
	m->quarantine_extents += n;
	sift_fsm_list_seal(page);
	sift_fsm_root_seal(root);
}

void
sift_fsm_release(Relation rel, const SiftExtent *ext, int n, FullTransactionId safexid)
{
	FsmCtx		c = sift_fsm_ctx_open(rel);

	while (n > 0)
	{
		int			take = Min(n, (int) SIFT_FSM_PAGE_CAP);
		SiftXlog	x;

		sift_fsm_reserve_release(rel, c.buf0, take);
		x = sift_xlog_start(rel);
		sift_fsm_release_in(rel, &x, ext, take, safexid);
		sift_xlog_done(&x);
		ext += take;
		n -= take;
	}
	sift_fsm_ctx_close(&c);
}

/* Every entry of the list that starts at head, with the blocks of its pages; the pages are checked as they are read */
void
sift_fsm_walk_list(Relation rel, BlockNumber head, uint16 size_class, ExtVec *out, ExtVec *pages)
{
	for (BlockNumber blk = head; blk != InvalidBlockNumber;)
	{
		Buffer		buf = sift_fsm_lock_list_page(rel, blk, size_class);
		Page		page = BufferGetPage(buf);

		for (uint32 i = 0; i < FSMP(page)->nentries; i++)
			sift_fsm_extvec_add(out, FSMP(page)->ext[i].start, FSMP(page)->ext[i].nblocks, FSMP(page)->ext[i].safexid);
		sift_fsm_extvec_add(pages, blk, 1, InvalidFullTransactionId);
		blk = FSMP(page)->next_blk;
		UnlockReleaseBuffer(buf);
		if (pages->n > (int) (RelationGetNumberOfBlocks(rel) + 1))
			SIFT_CORRUPT(rel, "An allocator list loops.");
	}
}

uint64
sift_fsm_reclaim(Relation rel)
{
	FsmCtx		c = sift_fsm_ctx_open(rel);
	uint64		n = sift_fsm_reclaim_locked(&c, NULL);

	sift_fsm_ctx_close(&c);
	return n;
}

uint64
sift_fsm_reclaim_exclusive(Relation rel)
{
	FsmCtx		c = sift_fsm_ctx_open(rel);
	uint64		n;

	c.exclusive = true;
	c.keep_empty = true;
	n = sift_fsm_reclaim_locked(&c, NULL);
	sift_fsm_ctx_close(&c);
	return n;
}

/* Counts the shrink that found the file busy (the relation lock was not free), or clears the count when the file was cut */
static void
note_shrink(Relation rel, bool done)
{
	Buffer		buf = sift_col_meta_lock(rel, BUFFER_LOCK_EXCLUSIVE);
	SiftXlog	x = sift_xlog_start(rel);
	Page		p0 = sift_xlog_register(&x, buf, 0);
	SiftColMetaPageData *m = SIFT_COL_META(p0);

	m->shrink_blocked = done ? 0 : Min(m->shrink_blocked + 1, PG_UINT16_MAX);
	if (done)
		m->last_shrink_at = GetCurrentTimestamp();
	sift_meta_seal(p0, m->common.struct_size);
	sift_xlog_done(&x);
}

/* The lock of the cut is only ever tried, never queued for: a reader that arrives meanwhile never waits behind the cut, which slips into the first gap between the statements of the readers */
static bool
try_lock_truncate(Relation rel, bool foreground)
{
	int			attempts = foreground ? SIFT_TAIL_SHRINK_FOREGROUND_TRIES : SIFT_TAIL_SHRINK_BACKGROUND_TRIES;

	while (!ConditionalLockRelation(rel, AccessExclusiveLock))
	{
		if (--attempts == 0)
			return false;
		CHECK_FOR_INTERRUPTS();
		(void) WaitLatch(MyLatch, WL_LATCH_SET | WL_TIMEOUT | WL_EXIT_ON_PM_DEATH, SIFT_TAIL_SHRINK_WAIT_MS, WAIT_EVENT_VACUUM_TRUNCATE);
		ResetLatch(MyLatch);
	}
	return true;
}

/*
 * The main fork cut to nblocks as RelationTruncate cuts it, and no other fork: the visibility map of a table is indexed by the virtual block of a row, which the size of the file does not bound, so
 * the cut of RelationTruncate would clear the bits of the vblocks past it (and log a full image of its last page)
 */
static void
truncate_main(Relation rel, BlockNumber nblocks)
{
	SMgrRelation reln = RelationGetSmgr(rel);
	ForkNumber	fork = MAIN_FORKNUM;
	BlockNumber old;

	reln->smgr_targblock = InvalidBlockNumber;
	for (int i = 0; i <= MAX_FORKNUM; i++)
		reln->smgr_cached_nblocks[i] = InvalidBlockNumber;
	old = smgrnblocks(reln, MAIN_FORKNUM);
	RelationPreTruncate(rel);
	Assert((MyProc->delayChkptFlags & (DELAY_CHKPT_START | DELAY_CHKPT_COMPLETE)) == 0);
	MyProc->delayChkptFlags |= DELAY_CHKPT_START | DELAY_CHKPT_COMPLETE;
	START_CRIT_SECTION();
	if (RelationNeedsWAL(rel))
	{
		xl_smgr_truncate xlrec = {.blkno = nblocks,.rlocator = rel->rd_locator,.flags = SMGR_TRUNCATE_HEAP};

		XLogBeginInsert();
		XLogRegisterData((char *) &xlrec, sizeof(xlrec));
		XLogFlush(XLogInsert(RM_SMGR_ID, XLOG_SMGR_TRUNCATE | XLR_SPECIAL_REL_UPDATE));
	}
	sift_smgrtruncate(RelationGetSmgr(rel), &fork, 1, &old, &nblocks);
	END_CRIT_SECTION();
	MyProc->delayChkptFlags &= ~(DELAY_CHKPT_START | DELAY_CHKPT_COMPLETE);
}

SiftTailShrinkResult
sift_fsm_tail_shrink(Relation rel)
{
	SiftTailShrinkResult res = {0};
	SiftFsmStats st;
	uint64		free_blocks = 0;
	BlockNumber hw,
				new_hw,
				nblocks;
	FsmCtx		c;

	/* The locks of the sweep and of the cut are WAL-logged for a standby, which takes a transaction id */
	if (sift_xid_unassignable() || sift_maint_standby_streams())
		return res;
	sift_fsm_meta_pool_drain(rel);
	if (sift_fsm_sweep_due(rel))
		(void) sift_fsm_sweep_orphans(rel);
	(void) sift_fsm_reclaim(rel);
	st = sift_fsm_check(rel);
	hw = sift_col_meta_read(rel).high_water;
	free_blocks = st.quarantine_blocks + st.meta_idle;
	for (int k = 0; k < SIFT_FSM_NCLASSES; k++)
		free_blocks += st.class_blocks[k];
	if ((free_blocks < SIFT_TAIL_SHRINK_MIN_BLOCKS || free_blocks * 100 < (uint64) hw * SIFT_TAIL_SHRINK_FREE_PCT) && RelationGetNumberOfBlocks(rel) <= hw)
		return res;
	if (!try_lock_truncate(rel, !AmAutoVacuumWorkerProcess() && MyBgworkerEntry == NULL))
	{
		res.lock_skipped = true;
		note_shrink(rel, false);
		return res;
	}
	c = sift_fsm_ctx_open(rel);
	c.exclusive = true;
	c.standby_horizon = sift_maint_standby_streams() ? sift_standby_safe_horizon(rel, false) : InvalidFullTransactionId;
	(void) sift_fsm_reclaim_locked(&c, &new_hw);
	sift_fsm_ctx_close(&c);
	nblocks = RelationGetNumberOfBlocks(rel);
	if (nblocks > new_hw)
	{
		truncate_main(rel, new_hw);
		res.blocks_truncated = nblocks - new_hw;
		note_shrink(rel, true);
	}
	UnlockRelation(rel, AccessExclusiveLock);
	return res;
}
