/*
 * core/tomb_write.c - what is written to the tombstone log (core/tomb.h): the entries a statement appends, and the rewrite VACUUM makes, built beside the appenders
 */
#include "postgres.h"

#include "access/generic_xlog.h"
#include "access/transam.h"
#include "storage/bufmgr.h"
#include "storage/lmgr.h"
#include "portability/instr_time.h"
#include "utils/rel.h"

#include "core/cmp.h"
#include "core/tomb.h"
#include "core/tomb_int.h"
#include "core/damage.h"
#ifdef SIFT_TESTING
#include "bm25/build.h"

/* the phase the rewrite of the log waits in for the advisory lock of sift.test_phase_gate + this, once the new generation is taken (table/load/write.c: 4 to 6) */
#define SIFT_TBL_PHASE_TOMB		7
#endif

#define corrupt(rel, ...) \
	SIFT_DAMAGE_DATA(rel, errmsg("relation \"%s\" has a corrupted tombstone log", RelationGetRelationName(rel)), errdetail(__VA_ARGS__))

/* Appends n entries to the segment e, under the lock of the caller: by the page, one record for the page and one for the header */
static void
tomb_append_seg(Relation rel, const SiftSegEntry *e, const SiftTomb *ents, int n)
{
	SiftWbufHeaderData hdr;

	while (n > 0)
	{
		Buffer		cur,
					hbuf;
		SiftPageEdit ed[2];
		Page		tp;
		int			lane,
					k;
		bool		crossed;

		sift_lane_read_header(rel, e, &hdr);
		if (!BufferIsValid(cur = sift_lane_lock_tail(rel, e->gen, &hdr.lanes[0].tail, 1, &lane)))
			corrupt(rel, "The tombstone segment is sealed.");
		if (SIFT_TOMB_PAGE(BufferGetPage(cur))->nent >= SIFT_TOMB_CAP)
			cur = sift_lane_grow(rel, e->gen, e->header_blk, lane, cur, SIFT_TOMB_PAGE_INIT, PG_UINT32_MAX, NULL, NULL, &crossed);
		hbuf = sift_lane_read_locked(rel, e->header_blk, BUFFER_LOCK_EXCLUSIVE);
		sift_page_edit_begin(&ed[0], hbuf, SIFT_PAGE_DATA_OFFSET);
		sift_page_edit_extra(&ed[0], (char *) &SIFT_TOMB_HDR(ed[0].page)->nentries - (char *) ed[0].page, sizeof(uint64));
		sift_page_edit_begin(&ed[1], cur, SIFT_PAGE_DATA_OFFSET + SIFT_TOMB_PAGE_INIT);
		tp = ed[1].page;
		k = Min((int) (SIFT_TOMB_CAP - SIFT_TOMB_PAGE(tp)->nent), n);
		memcpy(&SIFT_TOMB_ENTS(tp)[SIFT_TOMB_PAGE(tp)->nent], ents, k * sizeof(SiftTomb));
		SIFT_TOMB_PAGE(tp)->nent += k;
		SiftPageSetUsed(tp, SIFT_TOMB_PAGE_INIT + SIFT_TOMB_PAGE(tp)->nent * sizeof(SiftTomb));
		SIFT_TOMB_HDR(ed[0].page)->nentries += k;
		sift_page_edit_end(rel, ed, 2);
		UnlockReleaseBuffer(hbuf);
		UnlockReleaseBuffer(cur);
		ents += k;
		n -= k;
	}
}

void
sift_tomb_append_many(Relation rel, SiftTombLog *t, const SiftTomb *ents, int n)
{
	const SiftSegEntry *e;

	/* VACUUM replaces the segment under the exclusive form of this lock: the current one is read inside it */
	LockPage(rel, SIFT_TOMB_LOCK_BLKNO, ShareLock);
	sift_dir_read(rel, t->dir);
	if ((e = sift_tomb_find_seg(t->dir)) == NULL)
		corrupt(rel, "There is no tombstone segment.");
	tomb_append_seg(rel, e, ents, n);
	UnlockPage(rel, SIFT_TOMB_LOCK_BLKNO, ShareLock);
}

void
sift_tomb_append(Relation rel, SiftTombLog *t, uint64 key, TransactionId xid, CommandId cid, SiftTombKind kind,
					 uint16 flags, uint64 succ)
{
	SiftTomb	ent = {.key = key,.xid = xid,.cid = cid,.succ = succ,.kind = kind,.flags = flags};

	sift_tomb_append_many(rel, t, &ent, 1);
}

typedef struct TombSwap
{
	uint64		old_gen;
	SiftSegEntry fresh;
} TombSwap;

static void
swap_tomb(SiftMetaPageData *meta, SiftDirectoryData *dir, Page extra, void *arg)
{
	TombSwap   *a = arg;

	for (int i = 0; i < dir->nsegs; i++)
		if (dir->segs[i].gen == a->old_gen)
			dir->segs[i] = a->fresh;
}

/* The entries appended to the segment e since the log was read to its end, in the order of the log: palloc'd, n of them */
static SiftTomb *
tomb_read_tail(Relation rel, const SiftSegEntry *e, const SiftTombLog *t, uint32 *n, SiftExtent **ext, int *next)
{
	BlockNumber blk = t->last;
	uint32		from = t->lastcnt,
				cap = 64;
	SiftTomb   *out = palloc(cap * sizeof(SiftTomb));
	BlockNumber visited = 0,
				known = t->last;

	*n = 0;
	*next = 0;
	*ext = palloc(64 * sizeof(SiftExtent));
	if (!BlockNumberIsValid(blk))
	{
		SiftWbufHeaderData hdr;

		sift_lane_read_header(rel, e, &hdr);
		blk = hdr.lanes[0].head;
		from = 0;
	}
	while (BlockNumberIsValid(blk))
	{
		Buffer		buf = sift_lane_read_locked(rel, blk, BUFFER_LOCK_SHARE);
		const SiftTombPage *tp;

		sift_store_check_page(rel, buf, SIFT_PT_WBUFDATA, e->gen);
		tp = SIFT_TOMB_PAGE(BufferGetPage(buf));
		if (tp->nent > SIFT_TOMB_CAP || from > tp->nent)
			corrupt(rel, "Tombstone page %u lists %u entries, %u fit.", blk, tp->nent, (uint32) SIFT_TOMB_CAP);
		if (++visited > 4096 && visited > RelationGetNumberOfBlocks(rel))
			corrupt(rel, "The tombstone pages form a loop.");
		if (*n + (tp->nent - from) > cap)
		{
			cap = Max(cap * 2, *n + tp->nent);
			out = repalloc_huge(out, (Size) cap * sizeof(SiftTomb));
		}
		if (blk != known)
		{
			if (*next >= 64 && (*next & (*next - 1)) == 0)
				*ext = repalloc(*ext, 2 * *next * sizeof(SiftExtent));
			(*ext)[(*next)++] = (SiftExtent) {blk, 1};
		}
		memcpy(out + *n, SIFT_TOMB_ENTS(BufferGetPage(buf)) + from, (tp->nent - from) * sizeof(SiftTomb));
		*n += tp->nent - from;
		blk = tp->next;
		UnlockReleaseBuffer(buf);
		from = 0;
	}
	return out;
}

/* The extents of the pages of the segment e up to the page last that the cache of the log read to (the header page included): what the swap releases of the old log, found with the appenders at work */
static SiftExtent *
old_extents(Relation rel, const SiftSegEntry *e, BlockNumber last, int *n)
{
	int			nb;
	BlockNumber *blks = sift_lane_blocks(rel, e, &nb);
	SiftExtent *ext = palloc((nb + 1) * sizeof(SiftExtent));

	for (int i = 0; i < nb; i++)
		if (blks[i] == last)
		{
			nb = i + 1;
			break;
		}
	if (!BlockNumberIsValid(last))
		nb = 0;
	blks = repalloc(blks, (nb + 1) * sizeof(BlockNumber));
	blks[nb++] = e->header_blk;
	sift_sort_u32(blks, nb);
	*n = 0;
	for (int i = 0; i < nb; i++)
		sift_extent_push(ext, n, blks[i], 1);
	pfree(blks);
	return ext;
}

/*
 * The new segment without the entries keep() refuses, built from the log as it was read, with the appenders at work: they share the lock of the log, which this
 * takes only to move what they appended meanwhile into the new segment and to switch it in (the time of that is the entries appended during the build, not the length
 * of the log). The new segment is in no directory until then.
 */
uint64
sift_tomb_rewrite(Relation rel, SiftTombLog *t, SiftTombKeep keep, void *arg)
{
	const SiftSegEntry *old;
	TombSwap	sw;
	uint64		gen,
				kept = 0;
	Buffer		hbuf,
				prev = InvalidBuffer;
	BlockNumber head = InvalidBlockNumber,
				last;
	uint32		npages = 0,
				ntail;
	SiftExtent *rel_ext;
	int			nrel,
				nbuilt,
				ntail_ext;
	SiftExtent *built,
			   *tail_ext;
	SiftTombWalk wk;
#ifdef SIFT_TESTING
	instr_time	hold,
				done;
#endif
	SiftTomb   *ent,
			   *tail;

	LockPage(rel, SIFT_TOMB_LOCK_BLKNO, ShareLock);
	sift_dir_read(rel, t->dir);
	if ((old = sift_tomb_find_seg(t->dir)) == NULL)
		corrupt(rel, "There is no tombstone segment.");
	sw.old_gen = old->gen;
	tomb_reset(t, NULL);			/* the cache is rebuilt from the segment */
	sift_tomb_sync_all(rel, t);
	tomb_flatten(t);
	gen = sift_store_new_gen(rel);
#ifdef SIFT_TESTING
	sift_test_phase_wait(SIFT_TBL_PHASE_TOMB);
#endif
	hbuf = sift_alloc_page(rel, SIFT_PT_WBUFHEADER, gen);
	sift_tomb_walk_begin(&wk, t);
	for (ent = sift_tomb_walk_next(&wk); ent != NULL;)
	{
		Buffer		nb = sift_alloc_page(rel, SIFT_PT_WBUFDATA, gen);
		SiftXlog state = sift_xlog_start(rel);
		Page		np,
					pp = NULL;
		SiftTombPage *tp;

		if (BufferIsValid(prev))
			pp = sift_xlog_register(&state, prev, 0);
		np = sift_xlog_register(&state, nb, 0);
		sift_lane_init_page(np, SIFT_TOMB_PAGE_INIT);
		tp = SIFT_TOMB_PAGE(np);
		for (; ent != NULL && tp->nent < SIFT_TOMB_CAP; ent = sift_tomb_walk_next(&wk))
			if (keep(ent, arg))
			{
				SIFT_TOMB_ENTS(np)[tp->nent++] = *ent;
				kept++;
			}
		SiftPageSetUsed(np, SIFT_TOMB_PAGE_INIT + tp->nent * sizeof(SiftTomb));
		if (pp)
			SIFT_TOMB_PAGE(pp)->next = BufferGetBlockNumber(nb);
		sift_xlog_finish(&state);
		if (BufferIsValid(prev))
			UnlockReleaseBuffer(prev);
		else
			head = BufferGetBlockNumber(nb);
		prev = nb;
		npages++;
	}
	sift_tomb_walk_end(&wk);
	if (!BufferIsValid(prev))		/* nothing kept: one empty page, a lane always has a head */
	{
		prev = sift_alloc_page(rel, SIFT_PT_WBUFDATA, gen);
		head = BufferGetBlockNumber(prev);
		{
			SiftXlog state = sift_xlog_start(rel);
			Page		np = sift_xlog_register(&state, prev, 0);

			sift_lane_init_page(np, SIFT_TOMB_PAGE_INIT);
			sift_xlog_finish(&state);
		}
		npages = 1;
	}
	last = BufferGetBlockNumber(prev);
	UnlockReleaseBuffer(prev);
	{
		SiftXlog state = sift_xlog_start(rel);
		Page		hp = sift_xlog_register(&state, hbuf, 0);

		sift_lane_init_header(hp, gen, head, 1);
		SiftLaneHdr(hp)->lanes[0].tail = last;
		SiftLaneHdr(hp)->lanes[0].npages = npages;
		SIFT_TOMB_HDR(hp)->magic = SIFT_TOMB_MAGIC;
		SIFT_TOMB_HDR(hp)->nentries = kept;
		SIFT_TOMB_HDR(hp)->chan = t->chan;
		SIFT_TOMB_HDR(hp)->keys = t->keys;
		SiftPageSetUsed(hp, MAXALIGN(sizeof(SiftWbufHeaderData)) + sizeof(SiftTombHdr));
		sift_xlog_finish(&state);
	}
	sw.fresh = (SiftSegEntry) {.gen = gen,.kind = old->kind,.flags = old->flags,.format = old->format,
	.header_blk = BufferGetBlockNumber(hbuf)};
	UnlockReleaseBuffer(hbuf);
	built = old_extents(rel, old, t->last, &nbuilt);
	UnlockPage(rel, SIFT_TOMB_LOCK_BLKNO, ShareLock);

	LockPage(rel, SIFT_TOMB_LOCK_BLKNO, ExclusiveLock);
#ifdef SIFT_TESTING
	INSTR_TIME_SET_CURRENT(hold);
#endif
	sift_dir_read(rel, t->dir);
	if ((old = sift_tomb_find_seg(t->dir)) == NULL || old->gen != sw.old_gen)
		corrupt(rel, "The tombstone segment was replaced while its rewrite was built.");
	tail = tomb_read_tail(rel, old, t, &ntail, &tail_ext, &ntail_ext);
	tomb_append_seg(rel, &sw.fresh, tail, ntail);
	kept += ntail;
	pfree(tail);
	nrel = nbuilt + ntail_ext;
	rel_ext = palloc(nrel * sizeof(SiftExtent));
	memcpy(rel_ext, built, nbuilt * sizeof(SiftExtent));
	memcpy(rel_ext + nbuilt, tail_ext, ntail_ext * sizeof(SiftExtent));
	sift_dir_update(rel, &(SiftDirChange) {.fn = swap_tomb,.arg = &sw,.release = rel_ext,.nrelease = nrel,.extra = InvalidBuffer});
	UnlockPage(rel, SIFT_TOMB_LOCK_BLKNO, ExclusiveLock);
#ifdef SIFT_TESTING
	INSTR_TIME_SET_CURRENT(done);
	INSTR_TIME_SUBTRACT(done, hold);
	elog(LOG, "sift tomb rewrite: exclusive hold %lld us, %u entries appended meanwhile, %llu kept",
		 (long long) INSTR_TIME_GET_MICROSEC(done), ntail, (unsigned long long) kept);
#endif
	pfree(rel_ext);
	pfree(built);
	pfree(tail_ext);
	return kept;
}
