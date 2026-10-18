/*
 * sift_table_store.c - the layout of a sift_table relation (docs/COL-TAM.md section 2): the per-relation cache, delta
 * segments (the row-format write buffer) and the tombstone segment. Pages and the directory are those of sift_store.h.
 *
 * The page chains (a delta segment's data pages, the tombstone log) are the lane 0 of the lane segments of sift_lane.h, which also
 * seals them and takes the segment directory swaps; what differs from the record segments of the BM25 and column indexes is the
 * page format (slots of rows, tombstone entries), the single lane (a row must keep its vblock = position in the chain) and that
 * rows go to any unsealed page a backend holds, not just the tail. Lock order as there: the chain tail, the newly allocated page,
 * then the segment header page. Appenders of rows hold their target page only; a reader never holds two buffers.
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

#include "sift_lane.h"
#include "sift_table.h"

#define OVF_PAYLOAD		(SIFT_PAGE_PAYLOAD - TBL_PAGE_INIT)

#define corrupt(rel, ...) \
	ereport(ERROR, \
			(errcode(ERRCODE_DATA_CORRUPTED), \
			 errmsg("sift_table relation \"%s\" is corrupted", RelationGetRelationName(rel)), \
			 errdetail(__VA_ARGS__)))

int			sift_tbl_delta_limit = 4096;	/* kB */

/* ---------- the per relation cache ---------- */

typedef struct CacheEnt
{
	Oid			relid;
	SiftTblRel *st;
} CacheEnt;

static HTAB *cache_tab;
static List *retired;			/* contexts of cache entries replaced while a scan may still use them: freed at transaction end */

static void
cache_inval(Datum arg, Oid relid)
{
	HASH_SEQ_STATUS seq;
	CacheEnt   *e;

	if (cache_tab == NULL)
		return;
	hash_seq_init(&seq, cache_tab);
	while ((e = hash_seq_search(&seq)) != NULL)
		if (relid == InvalidOid || e->relid == relid)
			e->st->invalid = true;
}

static void
cache_xact(XactEvent event, void *arg)
{
	HASH_SEQ_STATUS seq;
	CacheEnt   *e;

	if (event != XACT_EVENT_COMMIT && event != XACT_EVENT_ABORT)
		return;
	foreach_ptr(MemoryContextData, cxt, retired)
		MemoryContextDelete(cxt);
	list_free(retired);
	retired = NIL;
	if (cache_tab == NULL)
		return;
	hash_seq_init(&seq, cache_tab);
	while ((e = hash_seq_search(&seq)) != NULL)
		if (e->st->invalid)
		{
			MemoryContextDelete(e->st->cxt);
			hash_search(cache_tab, &e->relid, HASH_REMOVE, NULL);
		}
}

/* The backend's caches go before its exit (the scans that the group reads keep between statements are not leaks to the leak check) */
static void
cache_exit(int code, Datum arg)
{
	HASH_SEQ_STATUS seq;
	CacheEnt   *e;

	foreach_ptr(MemoryContextData, cxt, retired)
		MemoryContextDelete(cxt);
	retired = NIL;
	hash_seq_init(&seq, cache_tab);
	while ((e = hash_seq_search(&seq)) != NULL)
		MemoryContextDelete(e->st->cxt);
}

SiftTblRel *
sift_tbl_rel(Relation rel)
{
	Oid			relid = RelationGetRelid(rel);
	bool		found;
	CacheEnt   *e;

	if (cache_tab == NULL)
	{
		HASHCTL		ctl = {.keysize = sizeof(Oid),.entrysize = sizeof(CacheEnt),.hcxt = CacheMemoryContext};

		cache_tab = hash_create("sift_table relations", 16, &ctl, HASH_ELEM | HASH_BLOBS | HASH_CONTEXT);
		CacheRegisterRelcacheCallback(cache_inval, (Datum) 0);
		RegisterXactCallback(cache_xact, NULL);
		on_proc_exit(cache_exit, 0);
	}
	e = hash_search(cache_tab, &relid, HASH_ENTER, &found);
	if (found && (e->st->invalid || e->st->relnumber != rel->rd_locator.relNumber))
	{
		MemoryContext old = MemoryContextSwitchTo(TopMemoryContext);

		retired = lappend(retired, e->st->cxt);
		MemoryContextSwitchTo(old);
		found = false;
	}
	if (!found)
	{
		MemoryContext cxt = AllocSetContextCreate(CacheMemoryContext, "sift cache table", ALLOCSET_DEFAULT_SIZES);

		e->st = MemoryContextAllocZero(cxt, sizeof(SiftTblRel));
		e->st->relid = relid;
		e->st->relnumber = rel->rd_locator.relNumber;
		e->st->cxt = cxt;
		e->st->tgt_blk = InvalidBlockNumber;
		e->st->tomb_last = InvalidBlockNumber;
	}
	return e->st;
}

/* ---------- pages and segments ---------- */

static void
init_data_page(Page page, uint32 vblock, uint16 flags)
{
	sift_lane_init_page(page, TBL_PAGE_INIT);
	TBL_PAGE(page)->ovf_next = InvalidBlockNumber;
	TBL_PAGE(page)->vblock = vblock;
	TBL_PAGE(page)->flags = flags;
}

bool
sift_tbl_is_table(Relation rel)
{
	return rel->rd_tableam == sift_tbl_routine();
}

static bool
is_tomb(const SiftSegEntry *e)
{
	return e->kind != SIFT_SEG_IMMUTABLE && (e->flags & SIFT_TBL_SEG_TOMB);
}

static bool
is_delta(const SiftSegEntry *e)
{
	return e->kind != SIFT_SEG_IMMUTABLE && (e->flags & SIFT_TBL_SEG_DELTA);
}

static const SiftSegEntry *
find_seg(const SiftDirSnapshot *dir, bool (*pred) (const SiftSegEntry *), SiftSegKind kind)
{
	for (int i = 0; i < dir->nsegs; i++)
		if (pred(&dir->segs[i]) && (kind == 0 || dir->segs[i].kind == kind))
			return &dir->segs[i];
	return NULL;
}

/* The first layout of the relation: meta, directory, the tombstone segment */
typedef struct InitArg
{
	SiftSegEntry tomb;
} InitArg;

static void
init_dir(SiftMetaPageData *meta, SiftDirectoryData *dir, Page extra, void *arg)
{
	meta->flags |= SIFT_TBL_META_FLAG;
	dir->segs[dir->nsegs++] = ((InitArg *) arg)->tomb;
}

static SiftSegEntry
new_tomb_segment(Relation rel)
{
	uint64		gen = sift_store_new_gen(rel);
	Buffer		hbuf = sift_alloc_page(rel, SIFT_PT_MUTHEADER, gen);
	Buffer		dbuf = sift_alloc_page(rel, SIFT_PT_MUTDATA, gen);
	GenericXLogState *state = GenericXLogStart(rel);
	Page		hp = GenericXLogRegisterBuffer(state, hbuf, 0);
	Page		dp = GenericXLogRegisterBuffer(state, dbuf, 0);
	SiftSegEntry e = {.gen = gen,.kind = SIFT_SEG_MUTABLE_ACTIVE,.flags = SIFT_TBL_SEG_TOMB,.header_blk = BufferGetBlockNumber(hbuf)};

	sift_lane_init_header(hp, gen, BufferGetBlockNumber(dbuf), 1);
	TBL_TOMB_HDR(hp)->magic = SIFT_TBL_TOMB_MAGIC;
	SiftPageSetUsed(hp, MAXALIGN(sizeof(SiftMutHeaderData)) + sizeof(SiftTblTombHdr));
	sift_lane_init_page(dp, TBL_TOMB_PAGE_INIT);
	GenericXLogFinish(state);
	UnlockReleaseBuffer(hbuf);
	UnlockReleaseBuffer(dbuf);
	return e;
}

void
sift_tbl_ensure_init(Relation rel)
{
	SiftTblRel *st = sift_tbl_rel(rel);

	if (st->initialized)
		return;
	if (RelationGetNumberOfBlocks(rel) == 0)
	{
		static const SiftOptions none = {0};

		LockRelationForExtension(rel, ExclusiveLock);
		if (RelationGetNumberOfBlocks(rel) == 0)
		{
			SiftWriteTarget *wt = sift_wt_begin(rel, SIFT_WRITE_BULK);

			sift_store_init_meta(wt, &none, 0);
			sift_wt_finish(wt);
		}
		UnlockRelationForExtension(rel, ExclusiveLock);
	}
	sift_tbl_refresh(rel, st);
	if (find_seg(&st->dir, is_tomb, 0) == NULL)
	{
		InitArg		arg;

		LockPage(rel, SIFT_ACTIVE_LOCK_BLKNO, ExclusiveLock);
		sift_dir_read(rel, &st->dir);
		if (find_seg(&st->dir, is_tomb, 0) == NULL)
		{
			arg.tomb = new_tomb_segment(rel);
			sift_dir_update(rel, &(SiftDirChange) {.fn = init_dir,.arg = &arg,.extra = InvalidBuffer});
		}
		UnlockPage(rel, SIFT_ACTIVE_LOCK_BLKNO, ExclusiveLock);
		sift_tbl_refresh(rel, st);
	}
}

void
sift_tbl_refresh(Relation rel, SiftTblRel *st)
{
	SiftTblSeg *old = st->delta;
	int			nold = st->ndelta,
				n = 0;

	if (RelationGetNumberOfBlocks(rel) == 0)
	{
		st->initialized = false;
		st->dir.nsegs = 0;
		st->ndelta = 0;
		return;
	}
	sift_dir_read(rel, &st->dir);
	if (st->dir.nsegs > 0 && !(st->dir.meta.flags & SIFT_TBL_META_FLAG))	/* flagged with the first segment */
		corrupt(rel, "The metapage does not carry the sift_table flag (0x%x).", st->dir.meta.flags);
	st->initialized = true;
	for (int i = 0; i < st->dir.nsegs; i++)
		n += is_delta(&st->dir.segs[i]);
	st->delta = MemoryContextAllocZero(st->cxt, Max(n, 1) * sizeof(SiftTblSeg));
	st->ndelta = 0;
	for (int i = 0; i < st->dir.nsegs; i++)
	{
		const SiftSegEntry *e = &st->dir.segs[i];
		SiftTblSeg *s;

		if (!is_delta(e))
			continue;
		s = &st->delta[st->ndelta++];
		for (int j = 0; j < nold; j++)
			if (old[j].gen == e->gen)
				*s = old[j];
		s->gen = e->gen;
		s->hdr = e->header_blk;
		s->vbase = (uint64) e->sb_lo * SIFT_TBL_VBLOCKS_SB;
		s->vcap = (e->sb_hi - e->sb_lo + 1) * SIFT_TBL_VBLOCKS_SB;
	}
	if (old)
		pfree(old);
}

uint64
sift_tbl_nvblocks(Relation rel)
{
	SiftTblRel *st = sift_tbl_rel(rel);

	sift_tbl_refresh(rel, st);
	return st->initialized ? SIFT_TBL_NEXT_VBLOCK(&st->dir.meta) : 0;
}

/* ---------- delta segments ---------- */

typedef struct SwapArg
{
	uint64		sealed;			/* gen of the active delta being replaced, 0 for none */
	SiftSegEntry fresh;
	uint64		next_vblock;
} SwapArg;

static void
swap_delta(SiftMetaPageData *meta, SiftDirectoryData *dir, Page extra, void *arg)
{
	SwapArg    *a = arg;

	sift_lane_swap_active(dir, a->sealed, &a->fresh);
	SIFT_TBL_NEXT_VBLOCK(meta) = a->next_vblock;
}

static uint32
delta_vcap(void)
{
	uint32		pages = Min((uint64) sift_tbl_delta_limit * 1024 / BLCKSZ, SIFT_TBL_MAX_DELTA_PAGES);

	return Max(1, pages / SIFT_TBL_VBLOCKS_SB) * SIFT_TBL_VBLOCKS_SB;
}

/*
 * Replaces the active delta segment (if any, and if it is still the one the caller saw: seen) with a fresh one. The first page of
 * the fresh segment is returned for the caller's appends, flagged flags, its vblock in *vblock; InvalidBlockNumber when another
 * backend was first.
 */
static BlockNumber
create_delta(Relation rel, SiftTblRel *st, uint64 seen, uint16 flags, uint32 *vblock, uint64 *newgen)
{
	const SiftSegEntry *act;
	BlockNumber first = InvalidBlockNumber;
	SiftSegEntry old = {0};

	LockPage(rel, SIFT_ACTIVE_LOCK_BLKNO, ExclusiveLock);
	sift_dir_read(rel, &st->dir);
	act = find_seg(&st->dir, is_delta, SIFT_SEG_MUTABLE_ACTIVE);
	if ((act ? act->gen : 0) == seen)
	{
		SwapArg		arg = {0};
		uint32		vcap = delta_vcap();
		uint64		gen,
					vbase = TYPEALIGN(SIFT_TBL_VBLOCKS_SB, SIFT_TBL_NEXT_VBLOCK(&st->dir.meta));
		Buffer		hbuf,
					dbuf;
		GenericXLogState *state;
		Page		hp,
					dp;

		if (st->dir.nsegs >= SIFT_MAX_SEGMENTS - 2)
			ereport(ERROR,
					(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
					 errmsg("sift_table \"%s\" has %d segments", RelationGetRelationName(rel), st->dir.nsegs),
					 errdetail("Too many unsealed delta segments: a long-running transaction prevents compaction."),
					 errhint("Run VACUUM on the table once the old transactions end.")));
		if (act)
			old = *act;
		if (vbase + vcap > (uint64) MaxBlockNumber)
			ereport(ERROR,
					(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
					 errmsg("sift_table \"%s\" exhausted its TIDs", RelationGetRelationName(rel)),
					 errhint("Run VACUUM FULL on the table.")));
		gen = sift_store_new_gen(rel);
		hbuf = sift_alloc_page(rel, SIFT_PT_MUTHEADER, gen);
		dbuf = sift_alloc_page(rel, SIFT_PT_MUTDATA, gen);
		first = BufferGetBlockNumber(dbuf);
		state = GenericXLogStart(rel);
		hp = GenericXLogRegisterBuffer(state, hbuf, 0);
		dp = GenericXLogRegisterBuffer(state, dbuf, 0);
		sift_lane_init_header(hp, gen, first, 1);
		TBL_DELTA_HDR(hp)->magic = SIFT_TBL_DELTA_MAGIC;
		TBL_DELTA_HDR(hp)->vbase = vbase;
		TBL_DELTA_HDR(hp)->vcap = vcap;
		TBL_DELTA_HDR(hp)->npages = 1;
		TBL_DELTA_HDR(hp)->page[0] = first;
		SiftPageSetUsed(hp, TBL_DELTA_HDR_USED);
		init_data_page(dp, vbase, flags);
		*vblock = vbase;
		*newgen = gen;
		GenericXLogFinish(state);
		arg.sealed = act ? act->gen : 0;
		arg.next_vblock = vbase + vcap;
		arg.fresh = (SiftSegEntry) {.gen = gen,.kind = SIFT_SEG_MUTABLE_ACTIVE,.flags = SIFT_TBL_SEG_DELTA,
		.header_blk = BufferGetBlockNumber(hbuf),.sb_lo = vbase / SIFT_TBL_VBLOCKS_SB,
		.sb_hi = (vbase + vcap) / SIFT_TBL_VBLOCKS_SB - 1};
		UnlockReleaseBuffer(hbuf);
		UnlockReleaseBuffer(dbuf);
		sift_dir_update(rel, &(SiftDirChange) {.fn = swap_delta,.arg = &arg,.extra = InvalidBuffer});
	}
	UnlockPage(rel, SIFT_ACTIVE_LOCK_BLKNO, ExclusiveLock);
	if (old.gen)
		sift_lane_flag_sealed_pages(rel, &old);
	sift_tbl_refresh(rel, st);
	if (old.gen && sift_tbl_inline_seal)
		(void) sift_tbl_seal(rel, st, GetOldestNonRemovableTransactionId(rel), false);
	return first;
}

void
sift_tbl_close_delta(Relation rel, SiftTblRel *st, uint32 minpages)
{
	const SiftSegEntry *act;
	uint32		vblock;
	uint64		gen;

	sift_tbl_refresh(rel, st);
	if (!st->initialized || (act = find_seg(&st->dir, is_delta, SIFT_SEG_MUTABLE_ACTIVE)) == NULL)
		return;
	for (int i = 0; i < st->ndelta; i++)
		if (st->delta[i].gen == act->gen)
		{
			sift_tbl_seg_load(rel, &st->delta[i]);
			if (st->delta[i].npages < minpages)
				return;
		}
	(void) create_delta(rel, st, act->gen, 0, &vblock, &gen);
}

/* The page of delta segment seg for vblock vbase + idx (the table is extended from the header when it is behind) */
BlockNumber
sift_tbl_delta_page(Relation rel, SiftTblSeg *seg, uint32 idx)
{
	if (idx >= seg->npages)
	{
		Buffer		buf = sift_lane_read_locked(rel, seg->hdr, BUFFER_LOCK_SHARE);
		const SiftTblDeltaHdr *h;

		sift_store_check_page(rel, buf, SIFT_PT_MUTHEADER, seg->gen);
		h = TBL_DELTA_HDR(BufferGetPage(buf));
		seg->npages = h->npages;
		memcpy(seg->page, h->page, h->npages * sizeof(BlockNumber));
		UnlockReleaseBuffer(buf);
	}
	return idx < seg->npages ? seg->page[idx] : InvalidBlockNumber;
}

/* Is the delta segment act still open to new pages: its header is its own and the vblocks reserved are not all in use; *tail: its last page */
static bool
delta_open(Relation rel, const SiftSegEntry *act, BlockNumber *tail)
{
	Buffer		hbuf = sift_lane_read_locked(rel, act->header_blk, BUFFER_LOCK_SHARE);
	Page		hp = BufferGetPage(hbuf);
	bool		open = !PageIsNew(hp) && SiftPageGetOpaque(hp)->type == SIFT_PT_MUTHEADER && SiftPageGetOpaque(hp)->gen == act->gen &&
		TBL_DELTA_HDR(hp)->npages < TBL_DELTA_HDR(hp)->vcap;

	if (open)
		*tail = SiftLaneHdr(hp)->lanes[0].tail;
	UnlockReleaseBuffer(hbuf);
	return open;
}

typedef struct DeltaLink
{
	uint16		flags;
	uint32		vblock;
} DeltaLink;

/* sift_lane_grow's fill: the new page is the next vblock of the segment, and enters its page table */
static void
link_delta_page(Page cur, Page np, Page hp, void *arg)
{
	DeltaLink  *l = arg;
	SiftTblDeltaHdr *h = TBL_DELTA_HDR(hp);

	l->vblock = h->vbase + h->npages;
	init_data_page(np, l->vblock, l->flags);
	h->page[h->npages++] = SiftLaneHead(cur)->next;
}

/*
 * Links a page after the tail of the delta segment act, initialized, flagged flags, unlocked, its vblock in *vblock. InvalidBlockNumber
 * when the segment is sealed, full or gone. The segment is full when its vblocks are used up (delta_open under the tail lock, which
 * is what the growers of the chain hold).
 */
static BlockNumber
extend_delta(Relation rel, const SiftSegEntry *act, uint16 flags, uint32 *vblock)
{
	DeltaLink	link = {.flags = flags};
	BlockNumber tail;
	Buffer		cur;
	int			lane;
	bool		crossed;

	if (!delta_open(rel, act, &tail) || !BufferIsValid(cur = sift_lane_lock_tail(rel, act->gen, &tail, 1, &lane)))
		return InvalidBlockNumber;
	if (!delta_open(rel, act, &tail))
	{
		UnlockReleaseBuffer(cur);
		return InvalidBlockNumber;
	}
	cur = sift_lane_grow(rel, act->gen, act->header_blk, lane, cur, TBL_PAGE_INIT, PG_UINT32_MAX, link_delta_page, &link, &crossed);
	tail = BufferGetBlockNumber(cur);
	UnlockReleaseBuffer(cur);
	*vblock = link.vblock;
	return tail;
}

/*
 * Adds a page to the active delta segment (creating the segment when needed) and returns it, initialized and unlocked,
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
		act = find_seg(&st->dir, is_delta, SIFT_SEG_MUTABLE_ACTIVE);
		if (act && BlockNumberIsValid(blk = extend_delta(rel, act, flags, vblock)))
		{
			*gen = act->gen;
			return blk;
		}
		if (BlockNumberIsValid(blk = create_delta(rel, st, act ? act->gen : 0, flags, vblock, gen)))
			return blk;
	}
}

/* A page of the delta segment gen, which must still be the active one and have room; InvalidBlockNumber otherwise */
static BlockNumber
claim_page_in(Relation rel, SiftTblRel *st, uint64 gen, uint16 flags)
{
	const SiftSegEntry *act;
	uint32		vb;

	sift_tbl_refresh(rel, st);
	act = find_seg(&st->dir, is_delta, SIFT_SEG_MUTABLE_ACTIVE);
	return act && act->gen == gen ? extend_delta(rel, act, flags, &vb) : InvalidBlockNumber;
}

/* The record of TID offset off in a delta page (locked or a copy); NULL when there is none */
const SiftTblRec *
sift_tbl_page_rec(Page page, uint64 gen, uint32 off)
{
	const SiftTblPageHdr *ph = TBL_PAGE(page);

	if (PageIsNew(page) || SiftPageGetOpaque(page)->type != SIFT_PT_MUTDATA || SiftPageGetOpaque(page)->gen != gen ||
		(ph->flags & SIFT_TBL_PAGE_OVFDATA) || off < 1 || off > ph->nrows || ph->slot[off - 1] == 0)
		return NULL;
	return (const SiftTblRec *) (SiftPageGetData(page) + ph->slot[off - 1]);
}

/*
 * Writes the bytes of a row that does not fit a page into overflow pages of the delta segment gen (chained front to back); returns the
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

	if (npg >= delta_vcap())
		ereport(ERROR,
				(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
				 errmsg("row of sift_table \"%s\" is too large: %u bytes", RelationGetRelationName(rel), mtup->t_len),
				 errdetail("A row has to fit one delta segment of %u kB.", sift_tbl_delta_limit)));
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
		GenericXLogState *state;
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
		if (!PageIsNew(page) && SiftPageGetOpaque(page)->type == SIFT_PT_MUTDATA &&
			!(TBL_PAGE(page)->flags & (SIFT_TBL_PAGE_SEALED | SIFT_TBL_PAGE_OVFDATA)) &&
			TBL_PAGE(page)->nrows < SIFT_TBL_MAX_OFF && SiftPageGetUsed(page) + need <= SIFT_PAGE_PAYLOAD &&
			SiftPageGetOpaque(page)->gen == st->tgt_gen)
		{
			uint32		off;

			state = GenericXLogStart(rel);
			page = GenericXLogRegisterBuffer(state, buf, 0);
			ph = TBL_PAGE(page);
			at = SiftPageGetData(page) + SiftPageGetUsed(page);
			memset(at, 0, need);
			memcpy(at, &rec, sizeof(SiftTblRec));
			if (rec.inline_len)
				memcpy(at + sizeof(SiftTblRec), mtup, rec.inline_len);
			ph->slot[ph->nrows] = at - SiftPageGetData(page);
			off = ++ph->nrows;
			SiftPageSetUsed(page, SiftPageGetUsed(page) + need);
			ItemPointerSet(tid, ph->vblock, off);
			GenericXLogFinish(state);
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

			sift_store_check_page(rel, buf, SIFT_PT_MUTDATA, gen);
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
	*rv = (SiftTblRowVer) {.key = sift_key_from_tid(tid),.delta = true,.xmin = rec->xmin,.cmin = rec->cmin,.flags = rec->flags,
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
	ExecForceStoreHeapTuple(tup, slot, false);
	ExecMaterializeSlot(slot);
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
		for (int i = 0; i < st->ndelta; i++)
		{
			SiftTblSeg *seg = &st->delta[i];
			BlockNumber blk;
			Buffer		buf;
			const SiftTblRec *rec;

			if (vblock < seg->vbase || vblock >= seg->vbase + seg->vcap)
				continue;
			blk = sift_tbl_delta_page(rel, seg, vblock - seg->vbase);
			if (!BlockNumberIsValid(blk))
				return false;
			buf = sift_lane_read_locked(rel, blk, BUFFER_LOCK_SHARE);
			sift_store_check_page(rel, buf, SIFT_PT_MUTDATA, seg->gen);
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
sift_tbl_rec_flag(Relation rel, SiftTblRel *st, ItemPointer tid, uint16 set)
{
	BlockNumber vblock = ItemPointerGetBlockNumberNoCheck(tid);

	for (int i = 0; i < st->ndelta; i++)
	{
		SiftTblSeg *seg = &st->delta[i];
		BlockNumber blk;
		Buffer		buf;
		GenericXLogState *state;
		Page		page;

		if (vblock < seg->vbase || vblock >= seg->vbase + seg->vcap)
			continue;
		blk = sift_tbl_delta_page(rel, seg, vblock - seg->vbase);
		if (!BlockNumberIsValid(blk))
			break;
		buf = sift_lane_read_locked(rel, blk, BUFFER_LOCK_EXCLUSIVE);
		sift_store_check_page(rel, buf, SIFT_PT_MUTDATA, seg->gen);
		if (sift_tbl_page_rec(BufferGetPage(buf), seg->gen, ItemPointerGetOffsetNumberNoCheck(tid)) != NULL)
		{
			state = GenericXLogStart(rel);
			page = GenericXLogRegisterBuffer(state, buf, 0);
			((SiftTblRec *) (SiftPageGetData(page) + TBL_PAGE(page)->slot[ItemPointerGetOffsetNumberNoCheck(tid) - 1]))->flags |= set;
			GenericXLogFinish(state);
		}
		UnlockReleaseBuffer(buf);
		return;
	}
}

/* ---------- tombstones ---------- */

static int
cmp_tomb(const void *a, const void *b)
{
	const SiftTblTomb *x = a,
			   *y = b;

	if (x->key != y->key)
		return x->key < y->key ? -1 : 1;
	return x->seq < y->seq ? -1 : x->seq > y->seq;
}

static void
tomb_reset(SiftTblRel *st, const SiftSegEntry *e)
{
	st->tomb_gen = e ? e->gen : 0;
	st->tomb_hdr = e ? e->header_blk : InvalidBlockNumber;
	st->tomb_synced = 0;
	st->tomb_last = InvalidBlockNumber;
	st->tomb_lastcnt = 0;
	st->nents = st->nrecent = 0;
}

/* Merges the sorted b into the sorted array *a (capacity *cap) in place, from the back: the entries before the smallest key of b do not move
 * (b is newer: entries of one key keep their seq order) */
static void
merge_tombs(SiftTblRel *st, SiftTblTomb **a, uint32 *na, uint32 *cap, const SiftTblTomb *b, uint32 nb)
{
	uint32		i = *na,
				j = nb,
				k = *na + nb;

	if (k > *cap)
	{
		*cap = Max(64, Max(*cap * 2, k));
		*a = *a ? repalloc(*a, *cap * sizeof(SiftTblTomb)) : MemoryContextAlloc(st->cxt, *cap * sizeof(SiftTblTomb));
	}
	while (j > 0)
		(*a)[--k] = i > 0 && cmp_tomb(&(*a)[i - 1], &b[j - 1]) > 0 ? (*a)[--i] : b[--j];
	*na += nb;
}

#define TOMB_RECENT_MAX 512

/* Everything in the main array, in order (the vacuum rewrite walks it) */
static void
tomb_flatten(SiftTblRel *st)
{
	merge_tombs(st, &st->ents, &st->nents, &st->capents, st->recent, st->nrecent);
	st->nrecent = 0;
}

/* Adds freshly synced entries: sorted into the small recent array, which joins the main one when it grows */
static void
tomb_add(SiftTblRel *st, SiftTblTomb *batch, uint32 n)
{
	qsort(batch, n, sizeof(SiftTblTomb), cmp_tomb);
	merge_tombs(st, &st->recent, &st->nrecent, &st->caprecent, batch, n);
	if (st->nrecent > TOMB_RECENT_MAX)
		tomb_flatten(st);
}

void
sift_tbl_tomb_sync(Relation rel, SiftTblRel *st)
{
	const SiftSegEntry *e = find_seg(&st->dir, is_tomb, 0);
	Buffer		buf;
	uint64		n;
	BlockNumber blk;
	SiftTblTomb *batch = NULL;
	uint32		nbatch = 0,
				capbatch = 0;

	if (e == NULL)
	{
		tomb_reset(st, NULL);
		return;
	}
	if (e->gen != st->tomb_gen)
		tomb_reset(st, e);
	buf = sift_lane_read_locked(rel, e->header_blk, BUFFER_LOCK_SHARE);
	sift_store_check_page(rel, buf, SIFT_PT_MUTHEADER, e->gen);
	n = TBL_TOMB_HDR(BufferGetPage(buf))->nentries;
	blk = BlockNumberIsValid(st->tomb_last) ? st->tomb_last : SiftLaneHdr(BufferGetPage(buf))->lanes[0].head;
	UnlockReleaseBuffer(buf);
	if (n == st->tomb_synced)
		return;
	while (BlockNumberIsValid(blk))
	{
		const SiftTblTombPage *tp;
		uint32		from = blk == st->tomb_last ? st->tomb_lastcnt : 0;

		buf = sift_lane_read_locked(rel, blk, BUFFER_LOCK_SHARE);
		sift_store_check_page(rel, buf, SIFT_PT_MUTDATA, e->gen);
		tp = TBL_TOMB_PAGE(BufferGetPage(buf));
		if (nbatch + (tp->nent - from) > capbatch)
		{
			capbatch = Max(64, Max(capbatch * 2, nbatch + tp->nent));
			batch = batch ? repalloc(batch, capbatch * sizeof(SiftTblTomb)) : palloc(capbatch * sizeof(SiftTblTomb));
		}
		for (uint32 i = from; i < tp->nent; i++)
		{
			batch[nbatch] = TBL_TOMB_ENTS(BufferGetPage(buf))[i];
			batch[nbatch++].seq = st->tomb_synced++;
		}
		st->tomb_last = blk;
		st->tomb_lastcnt = tp->nent;
		blk = tp->next;
		UnlockReleaseBuffer(buf);
	}
	if (nbatch > 0)
		tomb_add(st, batch, nbatch);
	if (batch)
		pfree(batch);
}

/* The run of entries for key in the sorted array a: its first element and length */
static const SiftTblTomb *
find_run(const SiftTblTomb *a, uint32 na, uint64 key, uint32 *n)
{
	uint32		lo = 0,
				hi = na;

	*n = 0;
	if (na == 0)
		return NULL;
	while (lo < hi)
	{
		uint32		mid = (lo + hi) / 2;

		if (a[mid].key < key)
			lo = mid + 1;
		else
			hi = mid;
	}
	for (; lo + *n < na && a[lo + *n].key == key; (*n)++)
		;
	return a + lo;
}

/* Does the sorted array a hold an entry with lo <= key <= hi (find_run's start is the lower bound of lo whether or not lo is present) */
static bool
tomb_run_in(const SiftTblTomb *a, uint32 na, uint64 lo, uint64 hi)
{
	uint32		n;
	const SiftTblTomb *f;

	if (na == 0)
		return false;
	f = find_run(a, na, lo, &n);
	return f < a + na && f->key <= hi;
}

bool
sift_tbl_tomb_any(const SiftTblRel *st, uint64 lo, uint64 hi)
{
	return tomb_run_in(st->ents, st->nents, lo, hi) || tomb_run_in(st->recent, st->nrecent, lo, hi);
}

const SiftTblTomb *
sift_tbl_tomb_find(const SiftTblRel *st, uint64 key, int *n)
{
	uint32		nm,
				nr;
	const SiftTblTomb *m = find_run(st->ents, st->nents, key, &nm),
			   *r = find_run(st->recent, st->nrecent, key, &nr);
	SiftTblRel *w = (SiftTblRel *) st;

	*n = nm + nr;
	if (nr == 0)
		return nm ? m : NULL;
	if (nm == 0)
		return r;
	if (nm + nr > w->caprun)
	{
		if (w->run)
			pfree(w->run);
		w->caprun = Max(8, (nm + nr) * 2);
		w->run = MemoryContextAlloc(w->cxt, w->caprun * sizeof(SiftTblTomb));
	}
	memcpy(w->run, m, nm * sizeof(SiftTblTomb));
	memcpy(w->run + nm, r, nr * sizeof(SiftTblTomb));
	return w->run;
}

void
sift_tbl_tomb_append(Relation rel, SiftTblRel *st, uint64 key, TransactionId xid, CommandId cid, SiftTblTombKind kind,
					 uint64 succ)
{
	const SiftSegEntry *e;
	SiftTblTomb ent = {.key = key,.xid = xid,.cid = cid,.succ = succ,.kind = kind};
	SiftMutHeaderData hdr;
	Buffer		cur,
				hbuf;
	GenericXLogState *state;
	Page		tp,
				hp;
	int			lane;
	bool		crossed;

	/* VACUUM replaces the segment under the exclusive form of this lock: the current one is read inside it */
	LockPage(rel, SIFT_TBL_TOMB_LOCK_BLKNO, ShareLock);
	sift_dir_read(rel, &st->dir);
	if ((e = find_seg(&st->dir, is_tomb, 0)) == NULL)
		corrupt(rel, "There is no tombstone segment.");
	sift_lane_read_header(rel, e, &hdr);
	if (!BufferIsValid(cur = sift_lane_lock_tail(rel, e->gen, &hdr.lanes[0].tail, 1, &lane)))
		corrupt(rel, "The tombstone segment is sealed.");
	if (TBL_TOMB_PAGE(BufferGetPage(cur))->nent >= TBL_TOMB_CAP)
		cur = sift_lane_grow(rel, e->gen, e->header_blk, lane, cur, TBL_TOMB_PAGE_INIT, PG_UINT32_MAX, NULL, NULL, &crossed);
	hbuf = sift_lane_read_locked(rel, e->header_blk, BUFFER_LOCK_EXCLUSIVE);
	state = GenericXLogStart(rel);
	hp = GenericXLogRegisterBuffer(state, hbuf, 0);
	tp = GenericXLogRegisterBuffer(state, cur, 0);
	TBL_TOMB_ENTS(tp)[TBL_TOMB_PAGE(tp)->nent++] = ent;
	SiftPageSetUsed(tp, TBL_TOMB_PAGE_INIT + TBL_TOMB_PAGE(tp)->nent * sizeof(SiftTblTomb));
	TBL_TOMB_HDR(hp)->nentries++;
	GenericXLogFinish(state);
	UnlockReleaseBuffer(hbuf);
	UnlockReleaseBuffer(cur);
	UnlockPage(rel, SIFT_TBL_TOMB_LOCK_BLKNO, ShareLock);
}

/* Brings the page table of seg up to the header's now */
void
sift_tbl_seg_load(Relation rel, SiftTblSeg *seg)
{
	seg->npages = 0;
	(void) sift_tbl_delta_page(rel, seg, 0);
}

/* Copies the delta page of vblock into copy (BLCKSZ bytes); false when no delta segment holds it. *gen: its segment's generation */
bool
sift_tbl_load_vblock(Relation rel, SiftTblRel *st, BlockNumber vblock, Page copy, uint64 *gen)
{
	for (int retry = 0; retry < 2; retry++)
	{
		for (int i = 0; i < st->ndelta; i++)
		{
			SiftTblSeg *seg = &st->delta[i];
			BlockNumber blk;
			Buffer		buf;

			if (vblock < seg->vbase || vblock >= seg->vbase + seg->vcap)
				continue;
			blk = sift_tbl_delta_page(rel, seg, vblock - seg->vbase);
			if (!BlockNumberIsValid(blk))
				return false;
			buf = sift_lane_read_locked(rel, blk, BUFFER_LOCK_SHARE);
			sift_store_check_page(rel, buf, SIFT_PT_MUTDATA, seg->gen);
			memcpy(copy, BufferGetPage(buf), BLCKSZ);
			UnlockReleaseBuffer(buf);
			*gen = seg->gen;
			return true;
		}
		if (retry == 0 && (vblock >= SIFT_TBL_NEXT_VBLOCK(&st->dir.meta) || !st->initialized))
			sift_tbl_refresh(rel, st);
		else
			break;
	}
	return false;
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

uint64
sift_tbl_tomb_rewrite(Relation rel, SiftTblRel *st, SiftTblTombKeep keep, void *arg)
{
	const SiftSegEntry *old;
	TombSwap	sw;
	uint64		gen,
				kept = 0;
	Buffer		hbuf,
				prev = InvalidBuffer;
	BlockNumber head = InvalidBlockNumber,
				last;
	uint32		npages = 0;
	SiftExtent *rel_ext;
	int			nrel;

	LockPage(rel, SIFT_TBL_TOMB_LOCK_BLKNO, ExclusiveLock);
	sift_dir_read(rel, &st->dir);
	if ((old = find_seg(&st->dir, is_tomb, 0)) == NULL)
		corrupt(rel, "There is no tombstone segment.");
	sw.old_gen = old->gen;
	tomb_reset(st, NULL);			/* the cache is rebuilt from the segment under the lock */
	sift_tbl_tomb_sync(rel, st);
	tomb_flatten(st);
	gen = sift_store_new_gen(rel);
	hbuf = sift_alloc_page(rel, SIFT_PT_MUTHEADER, gen);
	for (uint32 i = 0; i < st->nents;)
	{
		Buffer		nb = sift_alloc_page(rel, SIFT_PT_MUTDATA, gen);
		GenericXLogState *state = GenericXLogStart(rel);
		Page		np,
					pp = NULL;
		SiftTblTombPage *tp;

		if (BufferIsValid(prev))
			pp = GenericXLogRegisterBuffer(state, prev, 0);
		np = GenericXLogRegisterBuffer(state, nb, 0);
		sift_lane_init_page(np, TBL_TOMB_PAGE_INIT);
		tp = TBL_TOMB_PAGE(np);
		for (; i < st->nents && tp->nent < TBL_TOMB_CAP; i++)
			if (keep(&st->ents[i], arg))
			{
				TBL_TOMB_ENTS(np)[tp->nent++] = st->ents[i];
				kept++;
			}
		SiftPageSetUsed(np, TBL_TOMB_PAGE_INIT + tp->nent * sizeof(SiftTblTomb));
		if (pp)
			TBL_TOMB_PAGE(pp)->next = BufferGetBlockNumber(nb);
		GenericXLogFinish(state);
		if (BufferIsValid(prev))
			UnlockReleaseBuffer(prev);
		else
			head = BufferGetBlockNumber(nb);
		prev = nb;
		npages++;
	}
	if (!BufferIsValid(prev))		/* nothing kept: one empty page, a lane always has a head */
	{
		prev = sift_alloc_page(rel, SIFT_PT_MUTDATA, gen);
		head = BufferGetBlockNumber(prev);
		{
			GenericXLogState *state = GenericXLogStart(rel);
			Page		np = GenericXLogRegisterBuffer(state, prev, 0);

			sift_lane_init_page(np, TBL_TOMB_PAGE_INIT);
			GenericXLogFinish(state);
		}
		npages = 1;
	}
	last = BufferGetBlockNumber(prev);
	UnlockReleaseBuffer(prev);
	{
		GenericXLogState *state = GenericXLogStart(rel);
		Page		hp = GenericXLogRegisterBuffer(state, hbuf, 0);

		sift_lane_init_header(hp, gen, head, 1);
		SiftLaneHdr(hp)->lanes[0].tail = last;
		SiftLaneHdr(hp)->lanes[0].npages = npages;
		TBL_TOMB_HDR(hp)->magic = SIFT_TBL_TOMB_MAGIC;
		TBL_TOMB_HDR(hp)->nentries = kept;
		SiftPageSetUsed(hp, MAXALIGN(sizeof(SiftMutHeaderData)) + sizeof(SiftTblTombHdr));
		GenericXLogFinish(state);
	}
	sw.fresh = (SiftSegEntry) {.gen = gen,.kind = SIFT_SEG_MUTABLE_ACTIVE,.flags = SIFT_TBL_SEG_TOMB,
	.header_blk = BufferGetBlockNumber(hbuf)};
	UnlockReleaseBuffer(hbuf);
	rel_ext = sift_lane_extents(rel, old, &nrel);
	sift_dir_update(rel, &(SiftDirChange) {.fn = swap_tomb,.arg = &sw,.release = rel_ext,.nrelease = nrel,.extra = InvalidBuffer});
	UnlockPage(rel, SIFT_TBL_TOMB_LOCK_BLKNO, ExclusiveLock);
	pfree(rel_ext);
	sift_tbl_refresh(rel, st);
	return kept;
}

Buffer
sift_tbl_lock_page(Relation rel, const SiftTblSeg *seg, uint32 idx)
{
	Buffer		buf = sift_lane_read_locked(rel, seg->page[idx], BUFFER_LOCK_EXCLUSIVE);

	sift_store_check_page(rel, buf, SIFT_PT_MUTDATA, seg->gen);
	return buf;
}
