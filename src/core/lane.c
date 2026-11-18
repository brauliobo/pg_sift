/*
 * core/lane.c - the lane chains of a write buffer; see core/lane.h. The code of bm25/wbuf.c that does not depend on
 * what the records are: the active segment is created and sealed under SIFT_ACTIVE_LOCK_BLKNO, appends take the tail of a lane, then the
 * newly allocated page, then the header page; sealing takes lane tails one at a time and never the header.
 */
#include "postgres.h"

#include "access/generic_xlog.h"
#include "access/rmgr.h"
#include "access/xloginsert.h"
#include "miscadmin.h"
#include "storage/bufmgr.h"
#include "storage/lmgr.h"
#include "mb/pg_wchar.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"

#include "core/lane.h"
#include "core/limits.h"
#include "core/cmp.h"

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

/* Allocates and initializes (logged) the pages of an empty active segment; not yet reachable */
static SiftSegEntry
new_segment(Relation index, Size hdrsize, uint16 format, uint16 flags)
{
	uint64		gen = sift_store_new_gen(index);
	Buffer		bufs[SIFT_LANES + 1];
	BlockNumber first;
	SiftSegEntry e = {.gen = gen, .kind = SIFT_SEG_WBUF_OPEN, .format = format, .flags = flags};

	bufs[0] = sift_alloc_page(index, SIFT_PT_WBUFHEADER, gen);
	first = sift_alloc_extent(index, SIFT_LANES, SIFT_PT_WBUFDATA, gen, bufs + 1);
	e.header_blk = BufferGetBlockNumber(bufs[0]);
	for (int i = 0; i <= SIFT_LANES; i += MAX_GENERIC_XLOG_PAGES)
	{
		int			n = Min(MAX_GENERIC_XLOG_PAGES, SIFT_LANES + 1 - i);
		SiftXlog state = sift_xlog_start(index);

		for (int j = 0; j < n; j++)
		{
			Page		page = sift_xlog_register(&state, bufs[i + j], 0);

			if (i + j == 0)
				sift_lane_init_header(page, gen, first, SIFT_LANES);
			else
				sift_lane_init_page(page, hdrsize);
		}
		sift_xlog_finish(&state);
	}
	for (int i = 0; i <= SIFT_LANES; i++)
		UnlockReleaseBuffer(bufs[i]);
	return e;
}

typedef struct SwapArg
{
	uint64		sealed;			/* 0: only add */
	SiftSegEntry fresh;
	uint32		hash;			/* the options of the first segment of an index (open_first) */
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

/* The first segment of an index with none: the metapage takes the options it is written under */
static void
open_first(SiftMetaPageData *meta, SiftDirectoryData *dir, Page extra, void *arg)
{
	if (!sift_segs_have_layout(dir->segs, dir->nsegs))
		meta->analyzer_hash = ((SwapArg *) arg)->hash;
	swap_active(meta, dir, extra, arg);
}

/* What the last failed maintenance step said (the note of sift.workers), NULL if there is none or it is not text of the database encoding */
static const char *
last_failure(const SiftDirSnapshot *snap)
{
	static char msg[SIFT_MAINT_ERRMSG_LEN];

	if ((snap->meta.flags & SIFT_META_FAILURES_MASK) == 0 || snap->meta.maint_failed_at == 0)
		return NULL;
	memcpy(msg, snap->meta.maint_errmsg, sizeof(msg));
	msg[sizeof(msg) - 1] = '\0';
	return pg_verifymbstr(msg, strlen(msg), true) && msg[0] ? msg : NULL;
}

/* Creates the active segment unless another backend did meanwhile; false when the directory is full */
static bool
create_active(Relation index, uint32 hash, Size hdrsize, uint16 format, uint16 flags)
{
	SiftDirSnapshot *snap;
	bool		room = true;

	LockPage(index, SIFT_ACTIVE_LOCK_BLKNO, ExclusiveLock);
	snap = sift_dir_read(index);
	if (find_kind(snap, SIFT_SEG_WBUF_OPEN) == NULL && (room = snap->nsegs < sift_dir_cap(&snap->meta)))
	{
		SwapArg		arg = {0};

		arg.hash = hash;
		arg.fresh = new_segment(index, hdrsize, format, flags);
		sift_dir_update(index, &(SiftDirChange) {.fn = open_first,.arg = &arg,.nadd = 1,.extra = InvalidBuffer});
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
				SiftXlog state = sift_xlog_start(index);

				SiftLaneHead(sift_xlog_register(&state, buf, 0))->flags |= SIFT_WBUFPAGE_SEALED;
				sift_xlog_finish(&state);
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

static uint64
seal_open(Relation index, bool rotate, bool force, Size hdrsize, uint16 format, uint16 flags, uint32 limit, bool (*has_records) (Relation, const SiftSegEntry *))
{
	SiftDirSnapshot *snap;
	const SiftSegEntry *act;
	SiftSegEntry sealed;
	SwapArg		arg = {0};

	LockPage(index, SIFT_ACTIVE_LOCK_BLKNO, ExclusiveLock);
	snap = sift_dir_read(index);
	act = find_kind(snap, SIFT_SEG_WBUF_OPEN);
	if (act != NULL)
	{
		SiftWbufHeaderData hdr;

		sift_lane_read_header(index, act, &hdr);
		if (rotate || (force ? has_records(index, act) : sift_lane_npages(&hdr) >= limit))
		{
			if (snap->nsegs < sift_dir_cap(&snap->meta))
			{
				sealed = *act;
				arg.sealed = act->gen;
				arg.fresh = new_segment(index, hdrsize, format, flags);
				sift_dir_update(index, &(SiftDirChange) {.fn = swap_active,.arg = &arg,.nadd = 1,.extra = InvalidBuffer});
			}
			else if (force)
			{
				/* no entry is free for the buffer that would follow: this one stays open and takes the writes, as it does when the size alone asks for a seal */
				const char *why = last_failure(snap);

				ereport(WARNING,
						(errmsg("sift index \"%s\": it has %d segments and its write buffer stays open", RelationGetRelationName(index), snap->nsegs),
						 why ? errdetail("The last maintenance step failed: %s", why) : 0,
						 errhint("Run sift.merge() on the index, and read sift.workers for what the background did.")));
			}
		}
	}
	UnlockPage(index, SIFT_ACTIVE_LOCK_BLKNO, ExclusiveLock);
	if (arg.sealed)
		sift_lane_flag_sealed(index, &sealed);
	pfree(snap);
	return arg.sealed;
}

uint64
sift_lane_seal(Relation index, bool force, Size hdrsize, uint16 format, uint16 flags, uint32 limit, bool (*has_records) (Relation, const SiftSegEntry *))
{
	return seal_open(index, false, force, hdrsize, format, flags, limit, has_records);
}

uint64
sift_lane_rotate(Relation index, Size hdrsize, uint16 format, uint16 flags)
{
	return seal_open(index, true, true, hdrsize, format, flags, 0, NULL);
}

void
sift_lane_alter_refuse(Relation index, const char *what)
{
	ereport(ERROR,
			(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
			 errmsg("cannot change the %s of index \"%s\" while it holds data", what, RelationGetRelationName(index)),
			 errdetail("Its segments were written with the options it has now: reads and writes on its table would fail until a REINDEX writes them again."),
			 errhint("CREATE INDEX CONCURRENTLY a new index with the options, then DROP INDEX %s: no scan is lost while it is built.",
					 quote_qualified_identifier(get_namespace_name(RelationGetNamespace(index)), RelationGetRelationName(index)))));
}

void
sift_lane_refresh(Relation index, uint32 hash, const char *what, Size hdrsize, uint16 format, uint16 flags, bool (*make_room) (Relation),
				  uint64 *gen, BlockNumber *hdr_blk, BlockNumber *tail)
{
	SiftDirSnapshot *snap;
	const SiftSegEntry *act;
	SiftWbufHeaderData hdr;

	for (;;)
	{
		snap = sift_dir_read(index);
		if (sift_dir_layout_differs(snap, hash))
			sift_lane_layout_error(index, what);
		if ((act = find_kind(snap, SIFT_SEG_WBUF_OPEN)) != NULL)
			break;
		if (!create_active(index, hash, hdrsize, format, flags) && !make_room(index))
		{
			const char *why;

			pfree(snap);
			snap = sift_dir_read(index);
			why = last_failure(snap);
			ereport(ERROR,
					(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
					 errmsg("sift index \"%s\" has %d segments that cannot be merged", RelationGetRelationName(index), snap->nsegs),
					 why ? errdetail("The last maintenance step failed: %s", why) :
					 errdetail("A merge only takes segments whose rows every running transaction can see."),
					 errhint("End the long-running transactions, run VACUUM, then run sift.merge() on the index; sift.workers shows what the background did.")));
		}
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
	e->special = *SiftPageGetOpaque(page);
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

/* The rule of sift_xlog_finish for an edit; the pages are in a critical section, so a break of it is a PANIC: the server restarts from the log, which has none of the change */
static void
edit_check(Relation index, const SiftPageEdit *e)
{
	const char *why = NULL;

	if (!sift_page_layout_ok(e->page))
		why = "the page is not laid out as a page of the index";
	else if (memcmp(&e->special, SiftPageGetOpaque(e->page), sizeof(e->special)) != 0)
		why = "a byte of the special space changed";
	else if (e->extra_len > 0 && e->extra_off + e->extra_len > BLCKSZ)
		why = "the logged region lies past the page";
	if (why != NULL)
		ereport(ERROR,	/* check-errors: allow */
				(errcode(ERRCODE_INTERNAL_ERROR),
				 errmsg("sift index \"%s\": a change of block %u was refused", RelationGetRelationName(index), BufferGetBlockNumber(e->buf)),
				 errdetail("%s.", why)));
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
		edit_check(index, &e[i]);
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
	SiftXlog state = sift_xlog_start(index);
	Page		cp = sift_xlog_register(&state, cur, 0);
	Page		np = sift_xlog_register(&state, nb, 0);
	Page		hp = sift_xlog_register(&state, hb, 0);
	SiftWbufHeaderData *hd = SiftLaneHdr(hp);

	SiftLaneHead(cp)->next = BufferGetBlockNumber(nb);
	sift_lane_init_page(np, hdrsize);
	hd->lanes[lane].tail = BufferGetBlockNumber(nb);
	hd->lanes[lane].npages++;
	*crossed = sift_lane_npages(hd) >= limit;
	if (fill)
		fill(cp, np, hp, arg);
	sift_xlog_finish(&state);
	UnlockReleaseBuffer(hb);
	UnlockReleaseBuffer(cur);
	return nb;
}

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
	it->skip = NULL;
	it->skipped = false;
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
	if (it->skip)
	{
		Buffer		buf = sift_lane_read_locked(it->index, blk, BUFFER_LOCK_SHARE);
		Page		src = BufferGetPage(buf);

		sift_store_check_page(it->index, buf, SIFT_PT_WBUFDATA, it->entry->gen);
		it->skipped = it->skip(it->skip_arg, src);
		memcpy(dst, src, it->skipped ? SIFT_PAGE_DATA_OFFSET + hdrsize : BLCKSZ);
		UnlockReleaseBuffer(buf);
		if (SiftPageGetUsed(dst) < hdrsize)
			SIFT_CORRUPT(it->index, "Write buffer block %u of segment %llu is truncated.", blk, (unsigned long long) it->entry->gen);
	}
	else
		sift_lane_copy_page(it->index, it->entry, blk, dst, hdrsize);
	it->blk = blk;
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
			SiftXlog state = sift_xlog_start(index);
			Page		page = sift_xlog_register(&state, buf, 0);

			for (int k = 0; k < ndead; k++)
				flag(page, offs[k]);
			sift_xlog_finish(&state);
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

SiftExtent *
sift_lane_extents(Relation index, const SiftSegEntry *entry, int *n)
{
	int			nb;
	BlockNumber *blks = sift_lane_blocks(index, entry, &nb);
	SiftExtent *ext = palloc((nb + 1) * sizeof(SiftExtent));

	blks = repalloc(blks, (nb + 1) * sizeof(BlockNumber));
	blks[nb++] = entry->header_blk;
	sift_sort_u32(blks, nb);
	*n = 0;
	for (int i = 0; i < nb; i++)
		sift_extent_push(ext, n, blks[i], 1);
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
