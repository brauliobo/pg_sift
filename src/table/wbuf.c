/*
 * table/wbuf.c - the write buffers of a sift_table: row records in page chains, overflow rows, the lookup of a row by TID
 */
#include "postgres.h"

#include "access/generic_xlog.h"
#include "access/heapam.h"
#include "access/htup_details.h"
#include "storage/bufmgr.h"
#include "storage/lmgr.h"
#include "storage/ipc.h"
#include "storage/procarray.h"
#include "utils/hsearch.h"
#include "utils/inval.h"
#include "utils/memutils.h"
#include "utils/rel.h"

#include "core/lane.h"
#include "table/table.h"
#include "table/store_int.h"

#define OVF_PAYLOAD		(SIFT_PAGE_PAYLOAD - TBL_PAGE_INIT)

static void
init_data_page(Page page, uint32 vblock, uint16 flags)
{
	sift_lane_init_page(page, TBL_PAGE_INIT);
	TBL_PAGE(page)->ovf_next = InvalidBlockNumber;
	TBL_PAGE(page)->vblock = vblock;
	TBL_PAGE(page)->flags = flags;
}

/* ---------- write buffers ---------- */

typedef struct SwapArg
{
	uint64		sealed;			/* gen of the open write buffer being replaced, 0 for none */
	SiftSegEntry fresh;
	uint64		next_vblock;
} SwapArg;

static void
swap_wbuf(SiftMetaPageData *meta, SiftDirectoryData *dir, Page extra, void *arg)
{
	SwapArg    *a = arg;

	sift_lane_swap_active(dir, a->sealed, &a->fresh);
	SIFT_TBL_NEXT_VBLOCK(meta) = a->next_vblock;
}

static uint32
wbuf_vcap(void)
{
	uint32		pages = Min((uint64) sift_tbl_write_buffer_limit * 1024 / BLCKSZ, SIFT_TBL_MAX_WBUF_PAGES);

	return Max(1, pages / SIFT_TBL_VBLOCKS_SB) * SIFT_TBL_VBLOCKS_SB;
}

/*
 * Replaces the open write buffer segment (if any, and if it is still the one the caller saw: seen) with a fresh one. The first page of
 * the fresh segment is returned for the caller's appends, flagged flags, its vblock in *vblock; InvalidBlockNumber when another
 * backend was first.
 */
static BlockNumber
create_wbuf(Relation rel, SiftTblRel *st, uint64 seen, uint16 flags, uint32 *vblock, uint64 *newgen)
{
	const SiftSegEntry *act;
	BlockNumber first = InvalidBlockNumber;
	SiftSegEntry old = {0};
	bool		waited = false;

again:
	LockPage(rel, SIFT_ACTIVE_LOCK_BLKNO, ExclusiveLock);
	sift_dir_read(rel, &st->dir);
	act = find_seg(&st->dir, is_wbuf, SIFT_SEG_WBUF_OPEN);
	if ((act ? act->gen : 0) == seen)
	{
		SwapArg		arg = {0};
		uint32		vcap = wbuf_vcap();
		uint64		gen,
					vbase = TYPEALIGN(SIFT_TBL_VBLOCKS_SB, Max(SIFT_TBL_NEXT_VBLOCK(&st->dir.meta), (uint64) sift_tbl_vblock_floor));
		Buffer		hbuf,
					dbuf;
		GenericXLogState *state;
		Page		hp,
					dp;

		/* the directory is full: whoever promotes and merges holds the maintenance lock, which is waited for (not under the active lock: that one is taken inside it) before this is an error */
		if (st->dir.nsegs >= sift_tbl_max_segments - 2 && !waited)
		{
			UnlockPage(rel, SIFT_ACTIVE_LOCK_BLKNO, ExclusiveLock);
			waited = true;
			sift_tbl_settle(rel, st, GetOldestNonRemovableTransactionId(rel), true);
			goto again;
		}
		if (st->dir.nsegs >= sift_tbl_max_segments - 2)
			ereport(ERROR,
					(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
					 errmsg("sift_table \"%s\" has %d segments", RelationGetRelationName(rel), st->dir.nsegs),
					 errdetail("Too many write buffers waiting for promotion: a write buffer that holds a deleted row VACUUM has not seen, or a speculative insertion, is promoted only after that is settled, and about %d MB of rows are in write buffers like that.",
							   (int) ((uint64) (sift_tbl_max_segments - 2) * sift_tbl_write_buffer_limit / 1024)),
					 errhint("Run VACUUM on the table, or end the transaction that wrote them.")));
		if (act)
			old = *act;
		if (vbase + vcap > (uint64) MaxBlockNumber)
			ereport(ERROR,
					(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
					 errmsg("sift_table \"%s\" exhausted its TIDs", RelationGetRelationName(rel)),
					 errhint("Run VACUUM FULL on the table.")));
		gen = sift_store_new_gen(rel);
		hbuf = sift_alloc_page(rel, SIFT_PT_WBUFHEADER, gen);
		dbuf = sift_alloc_page(rel, SIFT_PT_WBUFDATA, gen);
		first = BufferGetBlockNumber(dbuf);
		state = GenericXLogStart(rel);
		hp = GenericXLogRegisterBuffer(state, hbuf, 0);
		dp = GenericXLogRegisterBuffer(state, dbuf, 0);
		sift_lane_init_header(hp, gen, first, 1);
		TBL_WBUF_HDR(hp)->magic = SIFT_TBL_WBUF_MAGIC;
		TBL_WBUF_HDR(hp)->vbase = vbase;
		TBL_WBUF_HDR(hp)->vcap = vcap;
		TBL_WBUF_HDR(hp)->npages = 1;
		TBL_WBUF_HDR(hp)->page[0] = first;
		SiftPageSetUsed(hp, TBL_WBUF_HDR_USED);
		init_data_page(dp, vbase, flags);
		*vblock = vbase;
		*newgen = gen;
		GenericXLogFinish(state);
		arg.sealed = act ? act->gen : 0;
		arg.next_vblock = vbase + vcap;
		arg.fresh = (SiftSegEntry) {.gen = gen,.kind = SIFT_SEG_WBUF_OPEN,.flags = SIFT_TBL_SEG_WBUF,
		.header_blk = BufferGetBlockNumber(hbuf),.sb_lo = vbase / SIFT_TBL_VBLOCKS_SB,
		.sb_hi = (vbase + vcap) / SIFT_TBL_VBLOCKS_SB - 1};
		UnlockReleaseBuffer(hbuf);
		UnlockReleaseBuffer(dbuf);
		sift_dir_update(rel, &(SiftDirChange) {.fn = swap_wbuf,.arg = &arg,.extra = InvalidBuffer});
	}
	UnlockPage(rel, SIFT_ACTIVE_LOCK_BLKNO, ExclusiveLock);
	if (old.gen)
		sift_lane_flag_sealed_pages(rel, &old);
	sift_tbl_refresh(rel, st);
	if (old.gen && sift_tbl_inline_promote)
		sift_tbl_settle(rel, st, GetOldestNonRemovableTransactionId(rel), false);
	return first;
}

void
sift_tbl_close_wbuf(Relation rel, SiftTblRel *st, uint32 minpages)
{
	const SiftSegEntry *act;
	uint32		vblock;
	uint64		gen;

	sift_tbl_refresh(rel, st);
	if (!st->initialized || (act = find_seg(&st->dir, is_wbuf, SIFT_SEG_WBUF_OPEN)) == NULL)
		return;
	for (int i = 0; i < st->nwbuf; i++)
		if (st->wbuf[i].gen == act->gen)
		{
			sift_tbl_seg_load(rel, &st->wbuf[i]);
			if (st->wbuf[i].npages < minpages)
				return;
		}
	(void) create_wbuf(rel, st, act->gen, 0, &vblock, &gen);
}

/* The page of write buffer seg for vblock vbase + idx (the table is extended from the header when it is behind) */
BlockNumber
sift_tbl_wbuf_page(Relation rel, SiftTblSeg *seg, uint32 idx)
{
	if (idx >= seg->npages)
	{
		Buffer		buf = sift_lane_read_locked(rel, seg->hdr, BUFFER_LOCK_SHARE);
		const SiftTblWbufHdr *h;

		sift_store_check_page(rel, buf, SIFT_PT_WBUFHEADER, seg->gen);
		h = TBL_WBUF_HDR(BufferGetPage(buf));
		if (h->npages > SIFT_TBL_MAX_WBUF_PAGES)
			corrupt(rel, "The write buffer header lists %u pages, at most %d fit.", h->npages, SIFT_TBL_MAX_WBUF_PAGES);
		seg->npages = h->npages;
		memcpy(seg->page, h->page, h->npages * sizeof(BlockNumber));
		UnlockReleaseBuffer(buf);
	}
	return idx < seg->npages ? seg->page[idx] : InvalidBlockNumber;
}

/* Is the write buffer act still open to new pages: its header is its own and the vblocks reserved are not all in use; *tail: its last page */
static bool
wbuf_open(Relation rel, const SiftSegEntry *act, BlockNumber *tail)
{
	Buffer		hbuf = sift_lane_read_locked(rel, act->header_blk, BUFFER_LOCK_SHARE);
	Page		hp = BufferGetPage(hbuf);
	bool		open = !PageIsNew(hp) && SiftPageGetOpaque(hp)->type == SIFT_PT_WBUFHEADER && SiftPageGetOpaque(hp)->gen == act->gen &&
		TBL_WBUF_HDR(hp)->npages < TBL_WBUF_HDR(hp)->vcap;

	if (open)
		*tail = SiftLaneHdr(hp)->lanes[0].tail;
	UnlockReleaseBuffer(hbuf);
	return open;
}

typedef struct WbufLink
{
	uint16		flags;
	uint32		vblock;
} WbufLink;

/* sift_lane_grow's fill: the new page is the next vblock of the segment, and enters its page table */
static void
link_wbuf_page(Page cur, Page np, Page hp, void *arg)
{
	WbufLink  *l = arg;
	SiftTblWbufHdr *h = TBL_WBUF_HDR(hp);

	l->vblock = h->vbase + h->npages;
	init_data_page(np, l->vblock, l->flags);
	h->page[h->npages++] = SiftLaneHead(cur)->next;
}

/*
 * Links a page after the tail of the write buffer act, initialized, flagged flags, unlocked, its vblock in *vblock. InvalidBlockNumber
 * when the segment is sealed, full or gone. The segment is full when its vblocks are used up (wbuf_open under the tail lock, which
 * is what the growers of the chain hold).
 */
static BlockNumber
extend_wbuf(Relation rel, const SiftSegEntry *act, uint16 flags, uint32 *vblock)
{
	WbufLink	link = {.flags = flags};
	BlockNumber tail;
	Buffer		cur;
	int			lane;
	bool		crossed;

	if (!wbuf_open(rel, act, &tail) || !BufferIsValid(cur = sift_lane_lock_tail(rel, act->gen, &tail, 1, &lane)))
		return InvalidBlockNumber;
	if (!wbuf_open(rel, act, &tail))
	{
		UnlockReleaseBuffer(cur);
		return InvalidBlockNumber;
	}
	cur = sift_lane_grow(rel, act->gen, act->header_blk, lane, cur, TBL_PAGE_INIT, PG_UINT32_MAX, link_wbuf_page, &link, &crossed);
	tail = BufferGetBlockNumber(cur);
	UnlockReleaseBuffer(cur);
	*vblock = link.vblock;
	return tail;
}

/*
 * Adds a page to the open write buffer segment (creating the segment when needed) and returns it, initialized and unlocked,
 * its vblock in *vblock and its segment's generation in *gen.
 */
static BlockNumber
claim_page(Relation rel, SiftTblRel *st, uint16 flags, uint32 *vblock, uint64 *gen)
{
	for (;;)
	{
		const SiftSegEntry *act;
		BlockNumber blk;

		sift_tbl_refresh(rel, st);
		act = find_seg(&st->dir, is_wbuf, SIFT_SEG_WBUF_OPEN);
		if (act && BlockNumberIsValid(blk = extend_wbuf(rel, act, flags, vblock)))
		{
			*gen = act->gen;
			return blk;
		}
		if (BlockNumberIsValid(blk = create_wbuf(rel, st, act ? act->gen : 0, flags, vblock, gen)))
			return blk;
	}
}

/* A page of the write buffer gen, which must still be the active one and have room; InvalidBlockNumber otherwise */
static BlockNumber
claim_page_in(Relation rel, SiftTblRel *st, uint64 gen, uint16 flags)
{
	const SiftSegEntry *act;
	uint32		vb;

	sift_tbl_refresh(rel, st);
	act = find_seg(&st->dir, is_wbuf, SIFT_SEG_WBUF_OPEN);
	return act && act->gen == gen ? extend_wbuf(rel, act, flags, &vb) : InvalidBlockNumber;
}

/* The record of TID offset off in a write buffer page (locked or a copy); NULL when there is none */
const SiftTblRec *
sift_tbl_page_rec(Page page, uint64 gen, uint32 off)
{
	const SiftTblPageHdr *ph = TBL_PAGE(page);
	const char *end;
	const SiftTblRec *rec;

	if (PageIsNew(page) || SiftPageGetOpaque(page)->type != SIFT_PT_WBUFDATA || SiftPageGetOpaque(page)->gen != gen ||
		(ph->flags & SIFT_TBL_PAGE_OVFDATA) || off < 1 || off > ph->nrows)
		return NULL;
	if (off > SIFT_TBL_MAX_OFF)
		ereport(ERROR, (errcode(ERRCODE_DATA_CORRUPTED), errmsg("sift_table write buffer page counts %u rows, at most %d fit", ph->nrows, SIFT_TBL_MAX_OFF)));
	if (ph->slot[off - 1] == 0)
		return NULL;
	rec = (const SiftTblRec *) (SiftPageGetData(page) + ph->slot[off - 1]);
	end = (const char *) PageGetSpecialPointer(page);
	if ((const char *) (rec + 1) > end || rec->inline_len > end - (const char *) (rec + 1))
		ereport(ERROR, (errcode(ERRCODE_DATA_CORRUPTED), errmsg("sift_table write buffer page holds a record that reaches past the page")));
	return rec;
}

/*
 * Writes the bytes of a row that does not fit a page into overflow pages of the write buffer gen (chained front to back); returns the
 * first block, InvalidBlockNumber when the segment cannot take them (sealed meanwhile, or full: the caller moves to the next segment).
 * They must share the segment of the row's record: the record's reader checks them against its generation, and reclaiming a segment
 * must take every byte of its rows.
 */
static BlockNumber
write_overflow(Relation rel, SiftTblRel *st, uint64 gen, MinimalTuple mtup)
{
	uint32		npg = (mtup->t_len + OVF_PAYLOAD - 1) / OVF_PAYLOAD;
	BlockNumber *blks = palloc(npg * sizeof(BlockNumber));
	BlockNumber first = InvalidBlockNumber;

	if (npg >= wbuf_vcap())
		ereport(ERROR,
				(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
				 errmsg("row of sift_table \"%s\" is too large: %u bytes", RelationGetRelationName(rel), mtup->t_len),
				 errdetail("A row has to fit one write buffer of %u kB.", sift_tbl_write_buffer_limit)));
	for (uint32 i = 0; i < npg; i++)
		if (!BlockNumberIsValid(blks[i] = claim_page_in(rel, st, gen, SIFT_TBL_PAGE_OVFDATA)))
			goto done;
	for (uint32 i = 0; i < npg; i++)
	{
		Buffer		buf = sift_lane_read_locked(rel, blks[i], BUFFER_LOCK_EXCLUSIVE);
		GenericXLogState *state = GenericXLogStart(rel);
		Page		p = GenericXLogRegisterBuffer(state, buf, 0);
		uint32		n = Min(OVF_PAYLOAD, mtup->t_len - i * OVF_PAYLOAD);

		memcpy((char *) TBL_PAGE(p) + TBL_PAGE_INIT, (char *) mtup + i * OVF_PAYLOAD, n);
		TBL_PAGE(p)->ovf_next = i + 1 < npg ? blks[i + 1] : InvalidBlockNumber;
		SiftPageSetUsed(p, TBL_PAGE_INIT + n);
		GenericXLogFinish(state);
		UnlockReleaseBuffer(buf);
	}
	first = blks[0];
done:
	pfree(blks);
	return first;
}

void
sift_tbl_append(Relation rel, SiftTblRel *st, MinimalTuple mtup, TransactionId xmin, CommandId cmin, uint32 token,
				ItemPointer tid)
{
	SiftTblRec	rec = {.len = mtup->t_len,.xmin = xmin,.cmin = cmin,.token = token};
	Size		need;
	uint64		ovf_gen = 0;		/* the segment holding the overflow pages written so far */

	if (token)
		rec.flags |= SIFT_TBL_REC_SPEC;
	if (mtup->t_len > SIFT_TBL_ROW_MAX_INLINE)
		rec.flags |= SIFT_TBL_REC_OVF;
	else
		rec.inline_len = mtup->t_len;
	need = TBL_REC_SIZE(rec.inline_len);
	for (;;)
	{
		Buffer		buf;
		Page		page;
		SiftTblPageHdr *ph;
		char	   *at;

		if (!BlockNumberIsValid(st->tgt_blk))
		{
			uint32		vb;

			st->tgt_blk = claim_page(rel, st, 0, &vb, &st->tgt_gen);
		}
		if ((rec.flags & SIFT_TBL_REC_OVF) && ovf_gen != st->tgt_gen)
		{
			if (!BlockNumberIsValid(rec.aux = write_overflow(rel, st, st->tgt_gen, mtup)))
			{
				st->tgt_blk = InvalidBlockNumber;	/* the segment of the target is full or sealed: row and bytes go to the next */
				continue;
			}
			ovf_gen = st->tgt_gen;
		}
		buf = sift_lane_read_locked(rel, st->tgt_blk, BUFFER_LOCK_EXCLUSIVE);
		page = BufferGetPage(buf);
		if (!PageIsNew(page) && SiftPageGetOpaque(page)->type == SIFT_PT_WBUFDATA &&
			!(TBL_PAGE(page)->flags & (SIFT_TBL_PAGE_SEALED | SIFT_TBL_PAGE_OVFDATA)) &&
			TBL_PAGE(page)->nrows < SIFT_TBL_MAX_OFF && SiftPageGetUsed(page) + need <= SIFT_PAGE_PAYLOAD &&
			SiftPageGetOpaque(page)->gen == st->tgt_gen)
		{
			uint32		off;
			SiftPageEdit ed;
			Size		slot_off;

			ph = TBL_PAGE(page);
			slot_off = SIFT_PAGE_DATA_OFFSET + offsetof(SiftTblPageHdr, slot) + ph->nrows * sizeof(ph->slot[0]);
			sift_page_edit_begin(&ed, page, SIFT_PAGE_DATA_OFFSET + offsetof(SiftTblPageHdr, slot));
			at = SiftPageGetData(page) + SiftPageGetUsed(page);
			memset(at, 0, need);
			memcpy(at, &rec, sizeof(SiftTblRec));
			if (rec.inline_len)
				memcpy(at + sizeof(SiftTblRec), mtup, rec.inline_len);
			ph->slot[ph->nrows] = at - SiftPageGetData(page);
			off = ++ph->nrows;
			SiftPageSetUsed(page, SiftPageGetUsed(page) + need);
			ItemPointerSet(tid, ph->vblock, off);
			sift_page_edit_end(rel, buf, &ed, slot_off, sizeof(ph->slot[0]));
			UnlockReleaseBuffer(buf);
			return;
		}
		UnlockReleaseBuffer(buf);
		st->tgt_blk = InvalidBlockNumber;
	}
}

/* Reads the row bytes of a record (an overflow row from its pages) into a HeapTuple */
HeapTuple
sift_tbl_rec_tuple(Relation rel, const SiftTblRec *rec, uint64 gen, uint64 key)
{
	MinimalTuple mt = palloc(rec->len);
	HeapTuple	tup;
	BlockNumber blk;
	uint32		got = 0;

	if (!(rec->flags & SIFT_TBL_REC_OVF))
		memcpy(mt, (const char *) (rec + 1), rec->len);
	else
		for (blk = rec->aux; BlockNumberIsValid(blk) && got < rec->len;)
		{
			Buffer		buf = sift_lane_read_locked(rel, blk, BUFFER_LOCK_SHARE);
			Page		p = BufferGetPage(buf);
			uint32		n = Min(SiftPageGetUsed(p) - TBL_PAGE_INIT, rec->len - got);

			sift_store_check_page(rel, buf, SIFT_PT_WBUFDATA, gen);
			memcpy((char *) mt + got, (char *) TBL_PAGE(p) + TBL_PAGE_INIT, n);
			got += n;
			blk = TBL_PAGE(p)->ovf_next;
			UnlockReleaseBuffer(buf);
		}
	if (!(rec->flags & SIFT_TBL_REC_OVF) || got == rec->len)
		tup = heap_tuple_from_minimal_tuple(mt);
	else
		corrupt(rel, "The overflow pages of row %llu hold %u of %u bytes.", (unsigned long long) key, got, rec->len);
	pfree(mt);
	return tup;
}

void
sift_tbl_rowver(SiftTblRowVer *rv, ItemPointer tid, const SiftTblRec *rec)
{
	*rv = (SiftTblRowVer) {.key = sift_key_from_tid(tid),.wbuf = true,.xmin = rec->xmin,.cmin = rec->cmin,.flags = rec->flags,
	.token = rec->token};
}

void
sift_tbl_rowver_tuple(Relation rel, SiftTblRowVer *rv, const SiftTblRec *rec, uint64 gen, ItemPointer tid)
{
	rv->tuple = sift_tbl_rec_tuple(rel, rec, gen, rv->key);
	rv->tuple->t_self = *tid;
	rv->tuple->t_tableOid = RelationGetRelid(rel);
}

/* Hands a row to slot. The slot frees what it holds, and a scan runs in a per-tuple memory context that is reset under it, so the row
 * moves into the slot's own context (as the heap's buffer slots do) and the caller's copy is released */
void
sift_tbl_store(TupleTableSlot *slot, HeapTuple tup)
{
	sift_tbl_slot_store_tuple(slot, tup);
	heap_freetuple(tup);
}

static bool
fetch_row(Relation rel, SiftTblRel *st, ItemPointer tid, SiftTblRowVer *rv, bool lazy)
{
	BlockNumber vblock = ItemPointerGetBlockNumberNoCheck(tid);
	OffsetNumber off = ItemPointerGetOffsetNumberNoCheck(tid);

	if (off < 1 || off > SIFT_TBL_MAX_OFF)
		return false;
	for (int retry = 0; retry < 2; retry++)
	{
		for (int i = 0; i < st->nwbuf; i++)
		{
			SiftTblSeg *seg = &st->wbuf[i];
			BlockNumber blk;
			Buffer		buf;
			const SiftTblRec *rec;

			if (vblock < seg->vbase || vblock >= seg->vbase + seg->vcap)
				continue;
			blk = sift_tbl_wbuf_page(rel, seg, vblock - seg->vbase);
			if (!BlockNumberIsValid(blk))
				return false;
			buf = sift_lane_read_locked(rel, blk, BUFFER_LOCK_SHARE);
			sift_store_check_page(rel, buf, SIFT_PT_WBUFDATA, seg->gen);
			rec = sift_tbl_page_rec(BufferGetPage(buf), seg->gen, off);
			if (rec == NULL)
			{
				UnlockReleaseBuffer(buf);
				return false;
			}
			sift_tbl_rowver(rv, tid, rec);
			sift_tbl_rowver_tuple(rel, rv, rec, seg->gen, tid);
			UnlockReleaseBuffer(buf);
			return true;
		}
		if (retry == 0 && (vblock >= SIFT_TBL_NEXT_VBLOCK(&st->dir.meta) || !st->initialized))
			sift_tbl_refresh(rel, st);
		else
			break;
	}
	return sift_tbl_group_fetch(rel, st, tid, rv, lazy);
}

bool
sift_tbl_fetch_row(Relation rel, SiftTblRel *st, ItemPointer tid, SiftTblRowVer *rv)
{
	return fetch_row(rel, st, tid, rv, false);
}

bool
sift_tbl_fetch_lazy(Relation rel, SiftTblRel *st, ItemPointer tid, SiftTblRowVer *rv)
{
	return fetch_row(rel, st, tid, rv, true);
}

void
sift_tbl_fetch_tuple(Relation rel, SiftTblRel *st, ItemPointer tid, SiftTblRowVer *rv)
{
	if (rv->tuple == NULL && !fetch_row(rel, st, tid, rv, false))
		elog(ERROR, "could not read the row (%u,%u) of \"%s\"", ItemPointerGetBlockNumberNoCheck(tid),
			 ItemPointerGetOffsetNumberNoCheck(tid), RelationGetRelationName(rel));
}

void
sift_tbl_rec_flag(Relation rel, SiftTblRel *st, ItemPointer tid, uint16 set, uint16 clear)
{
	BlockNumber vblock = ItemPointerGetBlockNumberNoCheck(tid);

	for (int i = 0; i < st->nwbuf; i++)
	{
		SiftTblSeg *seg = &st->wbuf[i];
		BlockNumber blk;
		Buffer		buf;
		GenericXLogState *state;
		Page		page;
		SiftTblRec *rec;

		if (vblock < seg->vbase || vblock >= seg->vbase + seg->vcap)
			continue;
		blk = sift_tbl_wbuf_page(rel, seg, vblock - seg->vbase);
		if (!BlockNumberIsValid(blk))
			break;
		buf = sift_lane_read_locked(rel, blk, BUFFER_LOCK_EXCLUSIVE);
		sift_store_check_page(rel, buf, SIFT_PT_WBUFDATA, seg->gen);
		if (sift_tbl_page_rec(BufferGetPage(buf), seg->gen, ItemPointerGetOffsetNumberNoCheck(tid)) != NULL)
		{
			state = GenericXLogStart(rel);
			page = GenericXLogRegisterBuffer(state, buf, 0);
			rec = (SiftTblRec *) (SiftPageGetData(page) + TBL_PAGE(page)->slot[ItemPointerGetOffsetNumberNoCheck(tid) - 1]);
			rec->flags = (rec->flags | set) & ~clear;
			GenericXLogFinish(state);
		}
		UnlockReleaseBuffer(buf);
		return;
	}
}
