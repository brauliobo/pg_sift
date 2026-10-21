/*
 * core/lane.c - the lane chains of a write buffer; see core/lane.h. The code of bm25/wbuf.c that does not depend on
 * what the records are: the active segment is created and sealed under SIFT_ACTIVE_LOCK_BLKNO, appends take the tail of a lane, then the
 * newly allocated page, then the header page; sealing takes lane tails one at a time and never the header.
 */
#include "postgres.h"

#include "access/generic_xlog.h"
#include "access/rmgr.h"
#include "access/xloginsert.h"
#include "common/int.h"
#include "miscadmin.h"
#include "storage/bufmgr.h"
#include "storage/lmgr.h"
#include "utils/rel.h"

#include "core/lane.h"

/* ---------- pages ---------- */

void
sift_lane_init_page(Page page, Size hdrsize)
{
	memset(SiftLaneHead(page), 0, hdrsize);
	SiftLaneHead(page)->next = InvalidBlockNumber;
	SiftPageSetUsed(page, hdrsize);
}

void
sift_lane_init_header(Page page, uint64 gen, BlockNumber first, int nlanes)
{
	SiftWbufHeaderData *hdr = SiftLaneHdr(page);

	hdr->gen = gen;
	for (int l = 0; l < SIFT_LANES; l++)
		hdr->lanes[l] = l < nlanes ? (SiftLaneData) {first + l, first + l, 1} : (SiftLaneData) {InvalidBlockNumber, InvalidBlockNumber, 0};
	SiftPageSetUsed(page, sizeof(SiftWbufHeaderData));
}

uint32
sift_lane_npages(const SiftWbufHeaderData *hdr)
{
	uint32		n = 0;

	for (int l = 0; l < SIFT_LANES; l++)
		n += hdr->lanes[l].npages;
	return n;
}

Buffer
sift_lane_read_locked(Relation index, BlockNumber blk, int mode)
{
	Buffer		buf = ReadBuffer(index, blk);

	LockBuffer(buf, mode);
	return buf;
}

void
sift_lane_read_header(Relation index, const SiftSegEntry *e, SiftWbufHeaderData *hdr)
{
	Buffer		buf = sift_lane_read_locked(index, e->header_blk, BUFFER_LOCK_SHARE);

	sift_store_check_page(index, buf, SIFT_PT_WBUFHEADER, e->gen);
	*hdr = *SiftLaneHdr(BufferGetPage(buf));
	UnlockReleaseBuffer(buf);
}

static const SiftSegEntry *
find_kind(const SiftDirSnapshot *snap, SiftSegKind kind)
{
	for (int i = 0; i < snap->nsegs; i++)
		if (snap->segs[i].kind == kind)
			return &snap->segs[i];
	return NULL;
}

/* ---------- segment creation and sealing ---------- */

/* Allocates and initializes (logged) the pages of an empty active segment; not yet reachable */
static SiftSegEntry
new_segment(Relation index, Size hdrsize, uint16 format)
{
	uint64		gen = sift_store_new_gen(index);
	Buffer		bufs[SIFT_LANES + 1];
	BlockNumber first;
	SiftSegEntry e = {.gen = gen, .kind = SIFT_SEG_WBUF_OPEN, .format = format};

	bufs[0] = sift_alloc_page(index, SIFT_PT_WBUFHEADER, gen);
	first = sift_alloc_extent(index, SIFT_LANES, SIFT_PT_WBUFDATA, gen, bufs + 1);
	e.header_blk = BufferGetBlockNumber(bufs[0]);
	for (int i = 0; i <= SIFT_LANES; i += MAX_GENERIC_XLOG_PAGES)
	{
		int			n = Min(MAX_GENERIC_XLOG_PAGES, SIFT_LANES + 1 - i);
		GenericXLogState *state = GenericXLogStart(index);

		for (int j = 0; j < n; j++)
		{
			Page		page = GenericXLogRegisterBuffer(state, bufs[i + j], 0);

			if (i + j == 0)
				sift_lane_init_header(page, gen, first, SIFT_LANES);
			else
				sift_lane_init_page(page, hdrsize);
		}
		GenericXLogFinish(state);
	}
	for (int i = 0; i <= SIFT_LANES; i++)
		UnlockReleaseBuffer(bufs[i]);
	return e;
}

typedef struct SwapArg
{
	uint64		sealed;			/* 0: only add */
	SiftSegEntry fresh;
} SwapArg;

void
sift_lane_swap_active(SiftDirectoryData *dir, uint64 sealed, const SiftSegEntry *fresh)
{
	for (int i = 0; i < dir->nsegs; i++)
		if (dir->segs[i].gen == sealed)
			dir->segs[i].kind = SIFT_SEG_WBUF_SEALED;
	dir->segs[dir->nsegs++] = *fresh;
}

static void
swap_active(SiftMetaPageData *meta, SiftDirectoryData *dir, Page extra, void *arg)
{
	SwapArg    *a = arg;

	sift_lane_swap_active(dir, a->sealed, &a->fresh);
}

static void
check_room(Relation index, const SiftDirSnapshot *snap)
{
	if (snap->nsegs >= SIFT_MAX_SEGMENTS)
		ereport(ERROR,
				(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
				 errmsg("sift index \"%s\" has %d segments", RelationGetRelationName(index), snap->nsegs),
				 errhint("Run sift.merge() on the index.")));
}

/* Creates the active segment unless another backend did meanwhile; false when the directory is full */
static bool
create_active(Relation index, Size hdrsize, uint16 format)
{
	SiftDirSnapshot *snap = palloc(sizeof(SiftDirSnapshot));
	bool		room = true;

	LockPage(index, SIFT_ACTIVE_LOCK_BLKNO, ExclusiveLock);
	sift_dir_read(index, snap);
	if (find_kind(snap, SIFT_SEG_WBUF_OPEN) == NULL && (room = snap->nsegs < SIFT_MAX_SEGMENTS))
	{
		SwapArg		arg = {0};

		arg.fresh = new_segment(index, hdrsize, format);
		sift_dir_update(index, &(SiftDirChange) {.fn = swap_active,.arg = &arg,.extra = InvalidBuffer});
	}
	UnlockPage(index, SIFT_ACTIVE_LOCK_BLKNO, ExclusiveLock);
	pfree(snap);
	return room;
}

/* Seals the chain of every lane from `start` on (a lane's head for whole, else its tail as the header has it) up to its true tail, which
 * is flagged under its exclusive lock: an appender needs that lock on an unsealed tail to grow the lane, so none can after this */
static void
flag_chains(Relation index, const SiftSegEntry *entry, bool whole)
{
	SiftWbufHeaderData hdr;

	sift_lane_read_header(index, entry, &hdr);
	for (int l = 0; l < SIFT_LANES; l++)
	{
		BlockNumber blk = whole ? hdr.lanes[l].head : hdr.lanes[l].tail;

		while (BlockNumberIsValid(blk))
		{
			Buffer		buf = sift_lane_read_locked(index, blk, BUFFER_LOCK_EXCLUSIVE);
			Page		page;

			sift_store_check_page(index, buf, SIFT_PT_WBUFDATA, entry->gen);
			page = BufferGetPage(buf);
			blk = SiftLaneHead(page)->next;
			if ((whole || !BlockNumberIsValid(blk)) && !(SiftLaneHead(page)->flags & SIFT_WBUFPAGE_SEALED))
			{
				GenericXLogState *state = GenericXLogStart(index);

				SiftLaneHead(GenericXLogRegisterBuffer(state, buf, 0))->flags |= SIFT_WBUFPAGE_SEALED;
				GenericXLogFinish(state);
			}
			UnlockReleaseBuffer(buf);
		}
	}
}

void
sift_lane_flag_sealed(Relation index, const SiftSegEntry *entry)
{
	flag_chains(index, entry, false);
}

void
sift_lane_flag_sealed_pages(Relation index, const SiftSegEntry *entry)
{
	flag_chains(index, entry, true);
}

uint64
sift_lane_seal(Relation index, bool force, Size hdrsize, uint16 format, uint32 limit, bool (*has_records) (Relation, const SiftSegEntry *))
{
	SiftDirSnapshot *snap = palloc(sizeof(SiftDirSnapshot));
	const SiftSegEntry *act;
	SiftSegEntry sealed;
	SwapArg		arg = {0};

	LockPage(index, SIFT_ACTIVE_LOCK_BLKNO, ExclusiveLock);
	sift_dir_read(index, snap);
	act = find_kind(snap, SIFT_SEG_WBUF_OPEN);
	if (act != NULL)
	{
		SiftWbufHeaderData hdr;

		sift_lane_read_header(index, act, &hdr);
		if (force ? has_records(index, act) : sift_lane_npages(&hdr) >= limit)
		{
			if (force || snap->nsegs < SIFT_MAX_SEGMENTS)
			{
				check_room(index, snap);
				sealed = *act;
				arg.sealed = act->gen;
				arg.fresh = new_segment(index, hdrsize, format);
				sift_dir_update(index, &(SiftDirChange) {.fn = swap_active,.arg = &arg,.extra = InvalidBuffer});
			}
		}
	}
	UnlockPage(index, SIFT_ACTIVE_LOCK_BLKNO, ExclusiveLock);
	if (arg.sealed)
		sift_lane_flag_sealed(index, &sealed);
	pfree(snap);
	return arg.sealed;
}

/* ---------- append ---------- */

void
sift_lane_refresh(Relation index, uint32 hash, const char *what, Size hdrsize, uint16 format, bool (*make_room) (Relation),
				  uint64 *gen, BlockNumber *hdr_blk, BlockNumber *tail)
{
	SiftDirSnapshot *snap = palloc(sizeof(SiftDirSnapshot));
	const SiftSegEntry *act;
	SiftWbufHeaderData hdr;

	for (;;)
	{
		sift_dir_read(index, snap);
		if (snap->meta.analyzer_hash != hash)
			ereport(ERROR,
					(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
					 errmsg("index \"%s\" was built with other %s", RelationGetRelationName(index), what),
					 errhint("Use REINDEX to apply the new options.")));
		if ((act = find_kind(snap, SIFT_SEG_WBUF_OPEN)) != NULL)
			break;
		if (!create_active(index, hdrsize, format) && !make_room(index))
			ereport(ERROR,
					(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
					 errmsg("sift index \"%s\" has %d segments that cannot be merged", RelationGetRelationName(index),
							SIFT_MAX_SEGMENTS)));
	}
	sift_lane_read_header(index, act, &hdr);
	*gen = act->gen;
	*hdr_blk = act->header_blk;
	for (int l = 0; l < SIFT_LANES; l++)
		tail[l] = hdr.lanes[l].tail;
	pfree(snap);
}

/* A page appends may still go to: the hinted segment's, not sealed */
static bool
open_tail(Page page, uint64 gen)
{
	return !PageIsNew(page) && SiftPageGetOpaque(page)->type == SIFT_PT_WBUFDATA &&
		SiftPageGetOpaque(page)->gen == gen && !(SiftLaneHead(page)->flags & SIFT_WBUFPAGE_SEALED);
}

Buffer
sift_lane_lock_tail(Relation index, uint64 gen, const BlockNumber *tail, int nlanes, int *lane)
{
	int			start = (uint32) MyProcNumber % nlanes;

	for (int i = 0; i <= nlanes; i++)
	{
		int			l = (start + i) % nlanes;
		Buffer		buf = ReadBuffer(index, tail[l]);

		if (i == nlanes)
			LockBuffer(buf, BUFFER_LOCK_EXCLUSIVE);
		else if (!ConditionalLockBuffer(buf))
		{
			ReleaseBuffer(buf);
			continue;
		}
		while (open_tail(BufferGetPage(buf), gen) && BlockNumberIsValid(SiftLaneHead(BufferGetPage(buf))->next))
		{
			BlockNumber next = SiftLaneHead(BufferGetPage(buf))->next;

			UnlockReleaseBuffer(buf);
			buf = sift_lane_read_locked(index, next, BUFFER_LOCK_EXCLUSIVE);
		}
		if (!open_tail(BufferGetPage(buf), gen))
		{
			UnlockReleaseBuffer(buf);
			return InvalidBuffer;
		}
		*lane = l;
		return buf;
	}
	pg_unreachable();
}

#define EDIT_HEAD_FRAGS	6			/* regions of the head bytes; with the extra region and the appended bytes, 16 chunks of a WAL record per page */
#define EDIT_FRAGS		(EDIT_HEAD_FRAGS + 2)
#define EDIT_GAP		4			/* equal bytes between two changed ones that are logged with them: a region costs 4 bytes of header */

void
sift_page_edit_begin(SiftPageEdit *e, Buffer buf, Size headlen)
{
	Page		page = BufferGetPage(buf);

	Assert(headlen <= SIFT_EDIT_HEAD);
	if (CritSectionCount == 0)		/* the first of the edits of a record: the room for all of them, outside a critical section */
		XLogEnsureRecordSpace(SIFT_EDIT_PAGES - 1, SIFT_EDIT_PAGES * EDIT_FRAGS * 2);
	e->buf = buf;
	e->page = page;
	e->headlen = headlen;
	e->lower = ((PageHeader) page)->pd_lower;
	e->extra_len = 0;
	memcpy(e->head, page, headlen);
#ifdef USE_ASSERTION_CHECKING
	memcpy(e->image, page, BLCKSZ);
#endif
	START_CRIT_SECTION();
}

void
sift_page_edit_extra(SiftPageEdit *e, Size off, Size len)
{
	e->extra_off = off;
	e->extra_len = len;
}

/* The regions (offset, length) of the page that changed; returns how many */
static int
edit_regions(const SiftPageEdit *e, uint16 (*frag)[2])
{
	const char *page = (const char *) e->page;
	int			nf = 0;

	for (Size i = 0; i < e->headlen;)
	{
		Size		end,
					j;

		if (page[i] == e->head[i])
		{
			i++;
			continue;
		}
		end = i + 1;
		for (j = end; j < e->headlen && j - end <= EDIT_GAP; j++)
			if (page[j] != e->head[j])
				end = j + 1;
		if (nf == EDIT_HEAD_FRAGS - 1)
			for (end = e->headlen; page[end - 1] == e->head[end - 1];)
				end--;
		frag[nf][0] = i;
		frag[nf++][1] = end - i;
		i = nf == EDIT_HEAD_FRAGS ? e->headlen : end;
	}
	if (e->extra_len > 0)
	{
		frag[nf][0] = e->extra_off;
		frag[nf++][1] = e->extra_len;
	}
	if (((PageHeader) page)->pd_lower > e->lower)
	{
		frag[nf][0] = e->lower;
		frag[nf++][1] = ((PageHeader) page)->pd_lower - e->lower;
	}
	return nf;
}

void
sift_page_edit_end(Relation index, SiftPageEdit *e, int n)
{
	uint16		frag[SIFT_EDIT_PAGES][EDIT_FRAGS][2];	/* offset, length */
	int			nf[SIFT_EDIT_PAGES];
	XLogRecPtr	lsn;

	Assert(n <= SIFT_EDIT_PAGES);
	for (int i = 0; i < n; i++)
	{
		nf[i] = edit_regions(&e[i], frag[i]);
#ifdef USE_ASSERTION_CHECKING
		for (int k = 0; k < nf[i]; k++)
			memcpy(e[i].image + frag[i][k][0], (char *) e[i].page + frag[i][k][0], frag[i][k][1]);
		Assert(memcmp(e[i].image, e[i].page, BLCKSZ) == 0);
#endif
		MarkBufferDirty(e[i].buf);
	}
	if (RelationNeedsWAL(index))
	{
		XLogBeginInsert();
		for (int i = 0; i < n; i++)
		{
			XLogRegisterBuffer(i, e[i].buf, REGBUF_STANDARD);
			for (int k = 0; k < nf[i]; k++)
			{
				XLogRegisterBufData(i, (char *) frag[i][k], sizeof(frag[i][k]));
				XLogRegisterBufData(i, (char *) e[i].page + frag[i][k][0], frag[i][k][1]);
			}
		}
		lsn = XLogInsert(RM_GENERIC_ID, 0);
		for (int i = 0; i < n; i++)
			PageSetLSN(e[i].page, lsn);
	}
	for (int i = 0; i < n; i++)
		END_CRIT_SECTION();
}

Buffer
sift_lane_grow(Relation index, uint64 gen, BlockNumber hdr_blk, int lane, Buffer cur, Size hdrsize, uint32 limit,
			   SiftLaneFill fill, void *arg, bool *crossed)
{
	Buffer		nb = sift_alloc_page(index, SIFT_PT_WBUFDATA, gen);
	Buffer		hb = sift_lane_read_locked(index, hdr_blk, BUFFER_LOCK_EXCLUSIVE);
	GenericXLogState *state = GenericXLogStart(index);
	Page		cp = GenericXLogRegisterBuffer(state, cur, 0);
	Page		np = GenericXLogRegisterBuffer(state, nb, 0);
	Page		hp = GenericXLogRegisterBuffer(state, hb, 0);
	SiftWbufHeaderData *hd = SiftLaneHdr(hp);

	SiftLaneHead(cp)->next = BufferGetBlockNumber(nb);
	sift_lane_init_page(np, hdrsize);
	hd->lanes[lane].tail = BufferGetBlockNumber(nb);
	hd->lanes[lane].npages++;
	*crossed = sift_lane_npages(hd) >= limit;
	if (fill)
		fill(cp, np, hp, arg);
	GenericXLogFinish(state);
	UnlockReleaseBuffer(hb);
	UnlockReleaseBuffer(cur);
	return nb;
}

/* ---------- walking ---------- */

void
sift_lane_copy_page(Relation index, const SiftSegEntry *e, BlockNumber blk, Page dst, Size hdrsize)
{
	Buffer		buf = sift_lane_read_locked(index, blk, BUFFER_LOCK_SHARE);

	sift_store_check_page(index, buf, SIFT_PT_WBUFDATA, e->gen);
	memcpy(dst, BufferGetPage(buf), BLCKSZ);
	UnlockReleaseBuffer(buf);
	if (SiftPageGetUsed(dst) < hdrsize)
		SIFT_CORRUPT(index, "Write buffer block %u of segment %llu is truncated.", blk, (unsigned long long) e->gen);
}

void
sift_lane_iter_init(SiftLaneIter *it, Relation index, const SiftSegEntry *entry)
{
	SiftWbufHeaderData hdr;

	it->index = index;
	it->entry = entry;
	it->lane = 0;
	it->nextblk = InvalidBlockNumber;
	sift_lane_read_header(index, entry, &hdr);
	for (int l = 0; l < SIFT_LANES; l++)
		it->head[l] = hdr.lanes[l].head;
}

bool
sift_lane_iter_next(SiftLaneIter *it, Page dst, Size hdrsize)
{
	BlockNumber blk = it->nextblk;

	while (!BlockNumberIsValid(blk) && it->lane < SIFT_LANES)
		blk = it->head[it->lane++];
	if (!BlockNumberIsValid(blk))
		return false;
	sift_lane_copy_page(it->index, it->entry, blk, dst, hdrsize);
	it->nextblk = SiftLaneHead(dst)->next;
	return true;
}

int64
sift_lane_bulkdelete(Relation index, const SiftSegEntry *entry, Size hdrsize, Size minrec, Page scratch,
					 SiftLaneCollect collect, SiftLaneFlag flag, void *arg,
					 IndexBulkDeleteCallback cb, void *cbstate, IndexBulkDeleteResult *stats)
{
	int			nb;
	BlockNumber *blks = sift_lane_blocks(index, entry, &nb);
	int64		removed = 0;
	Size	   *offs = palloc(SIFT_PAGE_PAYLOAD / minrec * sizeof(Size));
	ItemPointerData *tids = palloc(SIFT_PAGE_PAYLOAD / minrec * sizeof(ItemPointerData));

	for (int i = 0; i < nb; i++)
	{
		int			n,
					ndead = 0;

		sift_lane_copy_page(index, entry, blks[i], scratch, hdrsize);
		n = collect(arg, scratch, offs, tids);
		for (int k = 0; k < n; k++)
		{
			if (cb(&tids[k], cbstate))
				offs[ndead++] = offs[k];
			else
				stats->num_index_tuples++;
		}
		if (ndead > 0)
		{
			Buffer		buf = sift_lane_read_locked(index, blks[i], BUFFER_LOCK_EXCLUSIVE);
			GenericXLogState *state = GenericXLogStart(index);
			Page		page = GenericXLogRegisterBuffer(state, buf, 0);

			for (int k = 0; k < ndead; k++)
				flag(page, offs[k]);
			GenericXLogFinish(state);
			UnlockReleaseBuffer(buf);
			removed += ndead;
		}
		CHECK_FOR_INTERRUPTS();
	}
	pfree(offs);
	pfree(tids);
	pfree(blks);
	stats->tuples_removed += removed;
	return removed;
}

BlockNumber *
sift_lane_blocks(Relation index, const SiftSegEntry *entry, int *n)
{
	SiftWbufHeaderData hdr;
	int			cap = 64;
	BlockNumber *blks = palloc(cap * sizeof(BlockNumber));

	*n = 0;
	sift_lane_read_header(index, entry, &hdr);
	for (int l = 0; l < SIFT_LANES; l++)
		for (BlockNumber blk = hdr.lanes[l].head; BlockNumberIsValid(blk);)
		{
			Buffer		buf = sift_lane_read_locked(index, blk, BUFFER_LOCK_SHARE);

			sift_store_check_page(index, buf, SIFT_PT_WBUFDATA, entry->gen);
			if (*n == cap)
				blks = repalloc(blks, (cap *= 2) * sizeof(BlockNumber));
			blks[(*n)++] = blk;
			blk = SiftLaneHead(BufferGetPage(buf))->next;
			UnlockReleaseBuffer(buf);
		}
	return blks;
}

static int
blk_cmp(const void *a, const void *b)
{
	return pg_cmp_u32(*(const BlockNumber *) a, *(const BlockNumber *) b);
}

SiftExtent *
sift_lane_extents(Relation index, const SiftSegEntry *entry, int *n)
{
	int			nb;
	BlockNumber *blks = sift_lane_blocks(index, entry, &nb);
	SiftExtent *ext = palloc((nb + 1) * sizeof(SiftExtent));

	blks = repalloc(blks, (nb + 1) * sizeof(BlockNumber));
	blks[nb++] = entry->header_blk;
	qsort(blks, nb, sizeof(BlockNumber), blk_cmp);
	*n = 0;
	for (int i = 0; i < nb; i++)
		if (*n > 0 && ext[*n - 1].start + ext[*n - 1].nblocks == blks[i])
			ext[*n - 1].nblocks++;
		else
			ext[(*n)++] = (SiftExtent) {blks[i], 1};
	pfree(blks);
	return ext;
}

void
sift_lane_verify(Relation index, const SiftSegEntry *entry, Size hdrsize,
				 void (*check) (void *arg, Page page, BlockNumber blk), void *arg)
{
	SiftWbufHeaderData hdr;
	uint64		gen = entry->gen;
	Page		page = palloc(BLCKSZ);

	sift_lane_read_header(index, entry, &hdr);
	if (hdr.gen != gen)
		SIFT_CORRUPT(index, "Write buffer header of segment %llu carries generation %llu.", (unsigned long long) gen, (unsigned long long) hdr.gen);
	for (int l = 0; l < SIFT_LANES; l++)
	{
		BlockNumber blk = hdr.lanes[l].head;
		uint32		npages = 0;

		if (!BlockNumberIsValid(blk))
			SIFT_CORRUPT(index, "Lane %d of write buffer %llu has no pages.", l, (unsigned long long) gen);
		for (; BlockNumberIsValid(blk); npages++)
		{
			if (npages > RelationGetNumberOfBlocks(index))
				SIFT_CORRUPT(index, "Lane %d of write buffer %llu is a cycle.", l, (unsigned long long) gen);
			sift_lane_copy_page(index, entry, blk, page, hdrsize);
			check(arg, page, blk);
			blk = SiftLaneHead(page)->next;
		}
		/* appenders may have added pages since the header was read */
		if (npages < hdr.lanes[l].npages)
			SIFT_CORRUPT(index, "Lane %d of write buffer %llu has %u pages, its header %u.",
					l, (unsigned long long) gen, npages, hdr.lanes[l].npages);
	}
	pfree(page);
}
