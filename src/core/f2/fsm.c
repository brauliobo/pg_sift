/*
 * core/fsm.c - the allocator of format major 2 (core/fsm.h).
 *
 * Every public function takes the exclusive lock of block 0 first and keeps it to the end (the order of the locks is block 0, block 1, a page of a list), so the lists
 * only ever have one writer; a record of generic WAL here registers at most block 0, block 1 and two pages of a list. A step that moves an extent between lists is one
 * record per list: a crash between two of them leaves blocks that belong to no list and no part, which the orphan sweep of the index finds, never a block with two owners.
 */
#include "core/host.h"

#include "catalog/storage.h"
#include "storage/lmgr.h"
#include "utils/timestamp.h"

#include "col/f2/col_meta.h"
#include "core/f2/fsm.h"
#include "core/f2/f2_pace.h"
#include "core/f2/fsm/fsm_int.h"
#include "core/horizon.h"
#include "core/page_io.h"
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

static void
list_seal(Page page)
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
static int
class_floor(uint64 nblocks)
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
	if (FSMM(page)->magic != SIFT_FSM_MAGIC || !sift_page_sum_ok(page) || SLABS(page)->nslabs > SIFT_FSM_MAX_SLABS)
		SIFT_CORRUPT(rel, "The allocator root (block %u) is damaged.", BufferGetBlockNumber(buf));
}

Buffer
sift_fsm_lock_root(Relation rel)
{
	Buffer		buf = sift_blk_read(rel, SIFT_FSM_BLKNO, BUFFER_LOCK_EXCLUSIVE);

	sift_fsm_check_root(rel, buf);
	return buf;
}

static Buffer
lock_list_page(Relation rel, BlockNumber blk, uint16 size_class)
{
	Buffer		buf = sift_blk_read(rel, blk, BUFFER_LOCK_EXCLUSIVE);
	Page		page = BufferGetPage(buf);

	sift_page_expect(rel, page, blk, SIFT_PT_FSM_PAGE, 0, 0);
	if (FSMP(page)->magic != SIFT_FSM_MAGIC || FSMP(page)->size_class != size_class || !sift_page_sum_ok(page) || FSMP(page)->nentries > SIFT_FSM_PAGE_CAP)
		SIFT_CORRUPT(rel, "The allocator page at block %u is damaged.", blk);
	return buf;
}

static void
init_list_page(Page page, uint16 size_class, uint16 flags, BlockNumber next)
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
	Page		p0 = sift_xlog_register(x, c->buf0, 0);
	SiftColMetaPageData *m = SIFT_COL_META(p0);

	m->high_water = high_water;
	sift_meta_seal(p0, m->common.struct_size);
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
	SiftXlog	x = sift_xlog_start(c->rel);
	BlockNumber start = sift_fsm_high_water(c);

	if ((uint64) start + n >= SIFT_F2_LOCK_FLOOR)
		ereport(ERROR, (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED), errmsg("sift index \"%s\" cannot grow past %u blocks", RelationGetRelationName(c->rel), SIFT_F2_LOCK_FLOOR),
						errhint("Split the table, or partition it, so that each index is smaller.")));
	sift_fsm_set_high_water(c, &x, start + n);
	sift_xlog_finish(&x);
	return start;
}

/* Takes the last entry of the head page of the list of class c (it is not empty): the page is unlinked and its block freed when it was the last one */
SiftFsmExtent
sift_fsm_pop_class(FsmCtx *c, int cls)
{
	SiftXlog	x = sift_xlog_start(c->rel);
	Buffer		rootbuf = sift_fsm_lock_root(c->rel);
	Page		root = sift_xlog_register(&x, rootbuf, 0);
	SiftFsmMetaPageData *m = FSMM(root);
	BlockNumber head = m->class_head[cls];
	Buffer		hbuf;
	Page		hp;
	SiftFsmExtent e;

	if (head == InvalidBlockNumber || m->class_free_extents[cls] == 0)
		SIFT_CORRUPT(c->rel, "The allocator has no free extent of class %d.", cls);
	hbuf = lock_list_page(c->rel, head, cls);
	hp = sift_xlog_register(&x, hbuf, 0);
	e = FSMP(hp)->ext[--FSMP(hp)->nentries];
	memset(&FSMP(hp)->ext[FSMP(hp)->nentries], 0, sizeof(SiftFsmExtent));
	if (FSMP(hp)->nentries == 0)
	{
		m->class_head[cls] = FSMP(hp)->next_blk;
		sift_fsm_slab_put(c->rel, root, head);
	}
	m->class_free_extents[cls]--;
	list_seal(hp);
	sift_fsm_root_seal(root);
	sift_xlog_done(&x);
	return e;
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
			Buffer		hbuf = lock_list_page(c->rel, head, cls);

			page = sift_xlog_register(&x, hbuf, 0);
			room = SIFT_FSM_PAGE_CAP - FSMP(page)->nentries;
			if (room == 0)
				page = NULL;
		}
		else
			page = NULL;
		if (page == NULL)
		{
			BlockNumber blk = sift_fsm_slab_take(root);
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
			init_list_page(page, cls, 0, head);
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
		list_seal(page);
		sift_fsm_root_seal(root);
		sift_xlog_done(&x);
		ext += take;
		n -= take;
	}
}

/* The block of the lowest free extent of class cls (the head page of its list is popped from its end), false when there is none */
static bool
peek_lowest(FsmCtx *c, int cls, BlockNumber *start)
{
	Buffer		rootbuf = sift_fsm_lock_root(c->rel);
	SiftFsmMetaPageData *m = FSMM(BufferGetPage(rootbuf));
	bool		found = m->class_head[cls] != InvalidBlockNumber && m->class_free_extents[cls] > 0;

	if (found)
	{
		Buffer		hbuf = lock_list_page(c->rel, m->class_head[cls], cls);
		Page		hp = BufferGetPage(hbuf);

		*start = FSMP(hp)->ext[FSMP(hp)->nentries - 1].start;
		UnlockReleaseBuffer(hbuf);
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
		int			found = SIFT_FSM_NCLASSES;
		BlockNumber lowest = InvalidBlockNumber;

		UnlockReleaseBuffer(rootbuf);
		for (int k = cls; k < SIFT_FSM_NCLASSES; k++)
		{
			BlockNumber start;

			if (peek_lowest(c, k, &start) && (found == SIFT_FSM_NCLASSES || start < lowest))
			{
				found = k;
				lowest = start;
			}
		}
		if (found < SIFT_FSM_NCLASSES || !queued || round == 1)
			return found;
		sift_fsm_reclaim_locked(c, NULL);
	}
}

/* The first n blocks of a free extent of class k; the rest of it goes back to the lists, cut into the largest classes that fit */
static BlockNumber
carve_free(FsmCtx *c, int k, uint32 n)
{
	SiftFsmExtent e = sift_fsm_pop_class(c, k);
	uint64		at = (uint64) e.start + n,
				left = SIFT_FSM_CLASS_BLOCKS(k) - n;

	while (left > 0)
	{
		int			j = class_floor(left);
		SiftFsmExtent piece = {.start = (BlockNumber) at,.nblocks = SIFT_FSM_CLASS_BLOCKS(j),.safexid = InvalidFullTransactionId};

		sift_fsm_push_class(c, j, &piece, 1);
		at += piece.nblocks;
		left -= piece.nblocks;
	}
	return e.start;
}

/* n blocks in one extent: off a free extent of class k (SIFT_FSM_NCLASSES: there is none that holds them) or the end of the relation */
static SiftExtent
take_extent(FsmCtx *c, int k, uint32 n)
{
	SiftExtent	out = {.nblocks = n};

	out.start = k == SIFT_FSM_NCLASSES ? extend_high_water(c, n) : carve_free(c, k, n);
	if (sift_fsm_below_reserve(c))
		sift_fsm_grow_meta(c);
	return out;
}

static SiftExtent
alloc_locked(FsmCtx *c, uint32 nblocks)
{
	return take_extent(c, find_class(c, class_ceil(nblocks)), nblocks);
}

static FsmCtx
ctx_open(Relation rel)
{
	return (FsmCtx) {.rel = rel,.buf0 = sift_col_meta_lock(rel, BUFFER_LOCK_EXCLUSIVE)};
}

static void
ctx_close(FsmCtx *c)
{
	UnlockReleaseBuffer(c->buf0);
}

SiftExtent
sift_fsm_alloc(Relation rel, uint32 nblocks)
{
	FsmCtx		c = ctx_open(rel);
	SiftExtent	e = alloc_locked(&c, nblocks);

	ctx_close(&c);
	return e;
}

/* The lowest class of at most the n blocks still wanted whose free extent starts (with that many blocks) below max_blk: the class to carve and the class whose list it comes from, false when none does */
static bool
find_below(FsmCtx *c, uint64 npages, BlockNumber max_blk, int *cls, int *from)
{
	for (int j = class_floor((uint32) Min(npages, SIFT_FSM_MAX_EXTENT)); j >= 0; j--)
		for (int k = j; k < SIFT_FSM_NCLASSES; k++)
		{
			BlockNumber start;

			if (peek_lowest(c, k, &start) && (uint64) start + SIFT_FSM_CLASS_BLOCKS(j) <= max_blk)
			{
				*cls = j;
				*from = k;
				return true;
			}
		}
	return false;
}

int
sift_fsm_alloc_below(Relation rel, uint64 npages, BlockNumber max_blk, SiftExtent *out, int max)
{
	FsmCtx		c = ctx_open(rel);
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

				sift_fsm_push_class(&c, class_floor(out[i].nblocks), &back, 1);
			}
			n = 0;
			break;
		}
		out[n] = (SiftExtent) {.start = carve_free(&c, from, SIFT_FSM_CLASS_BLOCKS(cls)),.nblocks = SIFT_FSM_CLASS_BLOCKS(cls)};
		npages -= out[n].nblocks;
		n++;
	}
	ctx_close(&c);
	return n;
}

int
sift_fsm_alloc_stream(Relation rel, uint64 nblocks, SiftExtent *out, int max)
{
	FsmCtx		c = ctx_open(rel);
	int			n = 0;

	while (nblocks > 0)
	{
		uint32		top = (uint32) Min(nblocks, SIFT_FSM_MAX_EXTENT);
		int			cls = class_floor(top),
					k = find_class(&c, cls);

		if (n == max)
			ereport(ERROR, (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED), errmsg("sift: a stream of more than %d extents", max),
							errhint("Write the stream in several parts.")));
		out[n] = take_extent(&c, k, k == SIFT_FSM_NCLASSES ? top : SIFT_FSM_CLASS_BLOCKS(cls));
		nblocks -= out[n].nblocks;
		n++;
	}
	ctx_close(&c);
	return n;
}

BlockNumber
sift_fsm_alloc_meta_page(Relation rel)
{
	FsmCtx		c = ctx_open(rel);
	BlockNumber blk = sift_fsm_alloc_meta_locked(&c);

	if (sift_fsm_below_reserve(&c))
		sift_fsm_grow_meta(&c);
	ctx_close(&c);
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
	buf = lock_list_page(rel, tail, SIFT_FSM_CLASS_QUEUE);
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
		BlockNumber blk = sift_fsm_slab_take(root);
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
			Buffer		obuf = lock_list_page(c->rel, m->quarantine_tail, SIFT_FSM_CLASS_QUEUE);
			Page		old = sift_xlog_register(&x, obuf, 0);

			FSMP(old)->next_blk = blk;
			list_seal(old);
		}
		else
			m->quarantine_head = blk;
		m->quarantine_tail = blk;
		page = sift_xlog_register(&x, sift_blk_new(c->rel, blk), GENERIC_XLOG_FULL_IMAGE);
		init_list_page(page, SIFT_FSM_CLASS_QUEUE, SIFT_FSM_PAGE_DEFERRED, InvalidBlockNumber);
		list_seal(page);
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
	list_seal(page);
	sift_fsm_root_seal(root);
}

void
sift_fsm_release(Relation rel, const SiftExtent *ext, int n, FullTransactionId safexid)
{
	FsmCtx		c = ctx_open(rel);

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
	ctx_close(&c);
}

/* Every entry of the list that starts at head, with the blocks of its pages; the pages are checked as they are read */
void
sift_fsm_walk_list(Relation rel, BlockNumber head, uint16 size_class, ExtVec *out, ExtVec *pages)
{
	for (BlockNumber blk = head; blk != InvalidBlockNumber;)
	{
		Buffer		buf = lock_list_page(rel, blk, size_class);
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

/* lag: the standby-safe horizon an extent's id must precede too; invalid when the quarantine holds over a quarter of the index, which is then released at the raw horizon */
static FullTransactionId
quarantine_lag(Relation rel, const ExtVec *queue)
{
	uint64		held = 0;
	for (int i = 0; i < queue->n; i++)
		held += queue->v[i].nblocks;
	return held > RelationGetNumberOfBlocks(rel) / 4 ? InvalidFullTransactionId : sift_standby_safe_horizon(rel, false);
}

static bool
matured(const FsmCtx *c, FullTransactionId safexid, FullTransactionId lag)
{
	if (c->exclusive || !FullTransactionIdIsValid(safexid))
		return true;
	if (FullTransactionIdIsValid(lag) && !FullTransactionIdPrecedes(safexid, lag))
		return false;
	return FullTransactionIdIsValid(c->horizon) ? FullTransactionIdPrecedes(safexid, c->horizon) : sift_horizon_removable(c->rel, safexid);
}

/* The extents [start, start + n) cut into the largest classes that fit, each as the entry of its class */
static void
cut_into_classes(ExtVec *byclass, BlockNumber start, uint64 n)
{
	while (n > 0)
	{
		int			c = class_floor(n);

		sift_fsm_extvec_add(&byclass[c], start, SIFT_FSM_CLASS_BLOCKS(c), InvalidFullTransactionId);
		start += SIFT_FSM_CLASS_BLOCKS(c);
		n -= SIFT_FSM_CLASS_BLOCKS(c);
	}
}

/* Sorts the extents and joins the adjacent ones; an extent that is used up is dropped */
static void
coalesce(Relation rel, ExtVec *v)
{
	int			n = 0;

	for (int i = 0; i < v->n; i++)
		if (v->v[i].nblocks > 0)
			v->v[n++] = v->v[i];
	v->n = n;
	qsort(v->v, v->n, sizeof(SiftFsmExtent), sift_fsm_ext_cmp);
	n = 0;
	for (int i = 0; i < v->n;)
	{
		SiftFsmExtent run = v->v[i];
		uint64		end = (uint64) run.start + run.nblocks;

		for (i++; i < v->n && v->v[i].start <= end; i++)
		{
			if (v->v[i].start < end)
				SIFT_CORRUPT(rel, "Free extents overlap at block %u.", v->v[i].start);
			end += v->v[i].nblocks;
		}
		run.nblocks = (uint32) (end - run.start);
		v->v[n++] = run;
	}
	v->n = n;
}

static int
pages_for(int n)
{
	return (n + SIFT_FSM_PAGE_CAP - 1) / SIFT_FSM_PAGE_CAP;
}

/*
 * Rebuilds the lists: the matured extents of the queue join the free ones, adjacent runs are coalesced and cut into classes again, the rest of the queue is kept. The new pages
 * come from blocks the bitmaps hold free now and are written before the one record that switches the lists over (the old pages go back to the bitmaps in it), so a crash at any
 * point leaves the old lists. When the free metadata blocks do not hold the pages of all the lists, only the queue is rewritten first: its matured metadata pages go back to the bitmaps, which
 * gives the full pass that follows the room, and no slab is claimed for lists that the pages just given back would hold.
 */
static uint64
reclaim_pass(FsmCtx *c, BlockNumber *new_hw)
{
	Relation	rel = c->rel;
	bool		claimed = false;
	bool		meta_only = false;
	uint64		freed_before = 0;

	for (;;)
	{
		BlockNumber hw = new_hw != NULL ? sift_fsm_high_water(c) : 0;
		ExtVec		ready = {0}, queue = {0}, pages = {0}, data = {0}, pending = {0}, metafree = {0}, idle = {0};
		ExtVec		byclass[SIFT_FSM_NCLASSES] = {{0}};
		Buffer		rootbuf = sift_fsm_lock_root(rel);
		Page		root = BufferGetPage(rootbuf);
		SiftFsmMetaPageData *m = FSMM(root);
		int			newpages = 0;
		uint32		meta_free = m->meta_free_count;
		uint64		freed = freed_before;
		BlockNumber cut = InvalidBlockNumber;
		FullTransactionId max_matured = InvalidFullTransactionId,
					lag;

		if (!meta_only)
			for (int k = 0; k < SIFT_FSM_NCLASSES; k++)
				sift_fsm_walk_list(rel, m->class_head[k], k, &ready, &pages);
		sift_fsm_walk_list(rel, m->quarantine_head, SIFT_FSM_CLASS_QUEUE, &queue, &pages);
		lag = quarantine_lag(rel, &queue);
		for (int i = 0; i < queue.n; i++)
		{
			const SiftFsmExtent *q = &queue.v[i];
			bool		meta = sift_fsm_slab_of(SLABS(root), q->start) != NULL;

			if (!matured(c, q->safexid, lag))
				sift_fsm_extvec_add(&pending, q->start, q->nblocks, q->safexid);
			else if (meta_only && !meta)
				sift_fsm_extvec_add(&pending, q->start, q->nblocks, InvalidFullTransactionId);
			else
			{
				if (FullTransactionIdIsValid(q->safexid) && (!FullTransactionIdIsValid(max_matured) || FullTransactionIdFollows(q->safexid, max_matured)))
					max_matured = q->safexid;
				if (meta)
					sift_fsm_extvec_add(&metafree, q->start, q->nblocks, InvalidFullTransactionId);
				else
				{
					sift_fsm_extvec_add(&data, q->start, q->nblocks, InvalidFullTransactionId);
					freed += q->nblocks;
				}
			}
		}
		if (new_hw == NULL && (meta_only ? metafree.n == 0 : queue.n == pending.n))
		{
			UnlockReleaseBuffer(rootbuf);
			return freed_before;
		}
		for (int i = 0; i < ready.n; i++)
			sift_fsm_extvec_add(&data, ready.v[i].start, ready.v[i].nblocks, InvalidFullTransactionId);
		coalesce(rel, &data);
		if (new_hw != NULL && !claimed)
		{
			sift_fsm_slabs_idle(SLABS(root), meta_free, &idle);
			for (int i = 0; i < idle.n; i++)
			{
				sift_fsm_extvec_add(&data, idle.v[i].start, idle.v[i].nblocks, InvalidFullTransactionId);
				meta_free -= idle.v[i].nblocks;
			}
			coalesce(rel, &data);
		}
		for (int i = 0; i < data.n; i++)
		{
			BlockNumber start = data.v[i].start;

			if (new_hw != NULL && (uint64) start + data.v[i].nblocks == hw && data.v[i].nblocks >= SIFT_TAIL_SHRINK_MIN_BLOCKS)
				cut = start;
			else
				cut_into_classes(byclass, start, data.v[i].nblocks);
		}
		if (!meta_only)
			for (int k = 0; k < SIFT_FSM_NCLASSES; k++)
				newpages += pages_for(byclass[k].n);
		newpages += pages_for(pending.n);
		if (newpages > (int) meta_free)
		{
			UnlockReleaseBuffer(rootbuf);
			if (!meta_only && new_hw == NULL && metafree.n > 0)
				meta_only = true;
			else
			{
				sift_fsm_claim_slab(c);
				claimed = true;
			}
			continue;
		}

		{
			Buffer		wbuf;
			Page		wroot;
			SiftXlog	x;
			BlockNumber heads[SIFT_FSM_NCLASSES];
			BlockNumber qhead = InvalidBlockNumber, qtail = InvalidBlockNumber;
			BlockNumber *taken = palloc(Max(newpages, 1) * sizeof(BlockNumber));
			int			ntaken = 0;
			PGAlignedBlock scratch;

			(void) sift_wal_snapshot_conflict(rel, InvalidBlockNumber, max_matured);
			memcpy(scratch.data, root, BLCKSZ);
			sift_fsm_slabs_drop(scratch.data, &idle);
			for (int i = 0; i < newpages; i++)
				taken[i] = sift_fsm_slab_take(scratch.data);
			UnlockReleaseBuffer(rootbuf);

			for (int k = 0; k < SIFT_FSM_NCLASSES; k++)
			{
				BlockNumber next = InvalidBlockNumber;

				for (int p = (meta_only ? 0 : pages_for(byclass[k].n)) - 1; p >= 0; p--)
				{
					int			from = p * SIFT_FSM_PAGE_CAP,
								cnt = Min(byclass[k].n - from, (int) SIFT_FSM_PAGE_CAP);
					BlockNumber blk = taken[ntaken++];
					Page		pg;

					x = sift_xlog_start(rel);
					pg = sift_xlog_register(&x, sift_blk_new(rel, blk), GENERIC_XLOG_FULL_IMAGE);
					init_list_page(pg, k, 0, next);
					for (int j = 0; j < cnt; j++)
						FSMP(pg)->ext[j] = byclass[k].v[from + cnt - 1 - j];
					FSMP(pg)->nentries = cnt;
					list_seal(pg);
					sift_xlog_done(&x);
					next = blk;
				}
				heads[k] = next;
			}
			{
				BlockNumber next = InvalidBlockNumber;

				for (int p = pages_for(pending.n) - 1; p >= 0; p--)
				{
					int			from = p * SIFT_FSM_PAGE_CAP,
								cnt = Min(pending.n - from, (int) SIFT_FSM_PAGE_CAP);
					BlockNumber blk = taken[ntaken++];
					Page		pg;

					x = sift_xlog_start(rel);
					pg = sift_xlog_register(&x, sift_blk_new(rel, blk), GENERIC_XLOG_FULL_IMAGE);
					init_list_page(pg, SIFT_FSM_CLASS_QUEUE, SIFT_FSM_PAGE_DEFERRED, next);
					memcpy(FSMP(pg)->ext, &pending.v[from], cnt * sizeof(SiftFsmExtent));
					FSMP(pg)->nentries = cnt;
					list_seal(pg);
					sift_xlog_done(&x);
					if (qtail == InvalidBlockNumber)
						qtail = blk;
					next = blk;
				}
				qhead = next;
			}

			x = sift_xlog_start(rel);
			wbuf = sift_fsm_lock_root(rel);
			wroot = sift_xlog_register(&x, wbuf, 0);
			sift_fsm_slabs_drop(wroot, &idle);
			for (int i = 0; i < newpages; i++)
				if (sift_fsm_slab_take(wroot) != taken[i])
					SIFT_CORRUPT(rel, "The allocator changed under its own lock.");
			for (int i = 0; i < pages.n; i++)
				sift_fsm_slab_put(rel, wroot, pages.v[i].start);
			for (int i = 0; i < metafree.n; i++)
				for (uint32 b = 0; b < metafree.v[i].nblocks; b++)
					sift_fsm_slab_put(rel, wroot, metafree.v[i].start + b);
			m = FSMM(wroot);
			if (!meta_only)
				for (int k = 0; k < SIFT_FSM_NCLASSES; k++)
				{
					m->class_head[k] = heads[k];
					m->class_free_extents[k] = byclass[k].n;
				}
			m->quarantine_head = qhead;
			m->quarantine_tail = qtail;
			m->quarantine_extents = pending.n;
			m->quarantine_blocks = 0;
			for (int i = 0; i < pending.n; i++)
				m->quarantine_blocks += pending.v[i].nblocks;
			sift_fsm_root_seal(wroot);
			if (new_hw != NULL && cut != InvalidBlockNumber)
			{
				sift_fsm_set_high_water(c, &x, cut);
				sift_xlog_finish(&x);
				UnlockReleaseBuffer(wbuf);
			}
			else
				sift_xlog_done(&x);
		}
		for (int i = 0; i < metafree.n; i++)
			freed += metafree.v[i].nblocks;
		if (meta_only)
		{
			freed_before = freed;
			meta_only = false;
			continue;
		}
		if (new_hw != NULL)
			*new_hw = cut != InvalidBlockNumber ? cut : hw;
		return freed;
	}
}

uint64
sift_fsm_reclaim_locked(FsmCtx *c, BlockNumber *new_hw)
{
	bool		outer = !c->in_reclaim;
	uint64		freed;

	c->in_reclaim = true;
	freed = reclaim_pass(c, new_hw);
	if (outer && new_hw == NULL)
		(void) sift_fsm_recycle_empty_slabs(c);
	c->in_reclaim = !outer;
	return freed;
}

uint64
sift_fsm_reclaim(Relation rel)
{
	FsmCtx		c = ctx_open(rel);
	uint64		n = sift_fsm_reclaim_locked(&c, NULL);

	ctx_close(&c);
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

	(void) sift_fsm_reclaim(rel);
	st = sift_fsm_check(rel);
	hw = sift_col_meta_read(rel).high_water;
	free_blocks = st.quarantine_blocks + st.meta_idle;
	for (int k = 0; k < SIFT_FSM_NCLASSES; k++)
		free_blocks += st.class_blocks[k];
	if (free_blocks < SIFT_TAIL_SHRINK_MIN_BLOCKS || free_blocks * 100 < (uint64) hw * SIFT_TAIL_SHRINK_FREE_PCT)
		return res;
	if (!ConditionalLockRelation(rel, AccessExclusiveLock))
	{
		res.lock_skipped = true;
		note_shrink(rel, false);
		return res;
	}
	c = ctx_open(rel);
	c.exclusive = true;
	(void) sift_fsm_reclaim_locked(&c, &new_hw);
	ctx_close(&c);
	nblocks = RelationGetNumberOfBlocks(rel);
	if (nblocks > new_hw)
	{
		RelationTruncate(rel, new_hw);
		res.blocks_truncated = nblocks - new_hw;
		note_shrink(rel, true);
	}
	UnlockRelation(rel, AccessExclusiveLock);
	return res;
}
