/*
 * table/fetch.c - TID fetches, bitmap scans and TABLESAMPLE of a sift_table: the rows read by TID or by vblock
 */
#include "postgres.h"

#include "access/htup_details.h"
#include "access/tableam.h"
#include "access/tsmapi.h"
#include "access/xact.h"
#include "miscadmin.h"
#include "nodes/execnodes.h"
#include "storage/predicate.h"
#include "core/popcount.h"
#include "utils/memutils.h"
#include "utils/snapmgr.h"

#include "col/kern.h"
#include "table/table.h"
#include "core/visibility.h"
#include "table/scan_int.h"

typedef struct SiftTblFetch
{
	IndexFetchTableData base;
	SiftTblRel *st;
	bool		fresh;			/* the tombstones were synced by the begin and no fetch followed yet */
} SiftTblFetch;

IndexFetchTableData *
sift_tbl_index_fetch_begin(Relation rel SIFT_TAM_FLAGS_PARAM)
{
	SiftTblFetch *f = palloc0(sizeof(SiftTblFetch));

	f->base.rel = rel;
	f->st = sift_tbl_rel(rel);
	sift_tbl_auto_defrag_tick(rel, f->st);
	sift_tbl_refresh(rel, f->st);
	sift_tomb_sync(rel, &f->st->tomb);
	f->fresh = true;
	return &f->base;
}

void
sift_tbl_index_fetch_reset(IndexFetchTableData *data)
{
}

void
sift_tbl_index_fetch_end(IndexFetchTableData *data)
{
	pfree(data);
}

/*
 * Fetches tid, tests it against snapshot; the row goes into slot. *all_dead: no snapshot will ever see this TID again; sync: the tombstones may be stale.
 */
static bool
fetch_version(Relation rel, SiftTblRel *st, ItemPointer tid, Snapshot snapshot, TupleTableSlot *slot, bool *all_dead, bool sync)
{
	SiftTblRowVer rv;
	bool		dead;

	*all_dead = false;
	PredicateLockRelation(rel, snapshot);
	if (sync && (snapshot->snapshot_type == SNAPSHOT_DIRTY || snapshot->snapshot_type == SNAPSHOT_SELF))
		sift_tomb_sync(rel, &st->tomb);
	if (sift_tbl_deleted_by_self(st, snapshot, sift_key_from_tid(tid)))
		return false;
	if (!sift_tbl_fetch_lazy(rel, st, tid, &rv))
	{
		*all_dead = true;
		return false;
	}
	if (!sift_tbl_visible(rel, st, snapshot, &rv))
	{
		(void) sift_tbl_inserted_visible(snapshot, &rv, &dead);
		/* a version deleted or updated by a transaction that no snapshot can still see as running is dead for every snapshot: the b-tree marks its entry, and does not fetch it again */
		if (!dead)
		{
			SiftTblDel	del;

			sift_tbl_del_state(st, rv.key, &del);
			dead = (del.state == SIFT_TBL_DEL_DELETED || del.state == SIFT_TBL_DEL_UPDATED) && sift_tbl_xid_removable(rel, del.xid);
		}
		*all_dead = dead;
		sift_tbl_free_row(&rv);
		return false;
	}
	if (rv.tuple == NULL && sift_tbl_slot_is_lazy(slot) && sift_tbl_slot_locate(slot, rel, st, tid))
		return true;
	sift_tbl_fetch_tuple(rel, st, tid, &rv);
	sift_tbl_store(slot, rv.tuple);
	return true;
}

/*
 * The version of tid that the snapshot sees; chain: or the one of the chain a HOT update began at tid, which an index entry of tid stands for.
 * *all_dead: every version of the chain is dead for everyone (the index entry may go)
 */
static bool
fetch_visible(Relation rel, SiftTblRel *st, ItemPointer tid, Snapshot snapshot, TupleTableSlot *slot, bool *all_dead, bool sync, bool chain)
{
	ItemPointerData cur = *tid;
	bool		dead = true;

	for (;;)
	{
		bool		this_dead;
		uint64		succ;

		if (fetch_version(rel, st, &cur, snapshot, slot, &this_dead, sync))
		{
			if (all_dead)
				*all_dead = false;
			return true;
		}
		dead &= this_dead;
		if (!chain || !sift_tbl_hot_next(st, sift_key_from_tid(&cur), &succ))
			break;
		sift_key_to_tid(succ, &cur);
	}
	if (all_dead)
		*all_dead = dead;
	return false;
}

bool
sift_tbl_index_fetch_tuple(IndexFetchTableData *scan, ItemPointer tid, Snapshot snapshot, TupleTableSlot *slot,
						   bool *call_again, bool *all_dead)
{
	SiftTblFetch *f = (SiftTblFetch *) scan;

	bool		sync = !f->fresh;

	f->fresh = false;
	*call_again = false;
	return fetch_visible(scan->rel, f->st, tid, snapshot, slot, all_dead, sync, true);
}

/* The rows a page is asked for from which its tombstones are read at once (sift_tbl_filter_page_vers) */
#define PAGE_ROWS_MIN	16

typedef struct VblockTest
{
	Relation	rel;
	SiftTblRel *st;
	Snapshot	snapshot;
	Page		copy;			/* the write buffer page, NULL when the vblock is in a column group (the tombstones decide, and the insertion of a row with a version) */
	uint64		gen;
	SiftTblVerPage vp;			/* the versions of the rows of the page of a unit with unfrozen rows */
	bool		chain;			/* a TID stands for the versions of its HOT chain (the TIDs an index hands out), not for its own version alone */
} VblockTest;

/* Is the version of key, or one a HOT update of it led to, visible to the snapshot: what an index entry of the TID stands for */
static bool
chain_visible(VblockTest *t, uint64 key)
{
	uint64		succ;

	while (t->chain && t->st->tomb.hotm != NULL && sift_tbl_hot_next(t->st, key, &succ))
	{
		ItemPointerData tid;
		SiftTblRowVer rv;
		bool		vis;

		sift_key_to_tid(succ, &tid);
		key = succ;
		if (!sift_tbl_fetch_lazy(t->rel, t->st, &tid, &rv))
			continue;
		vis = sift_tbl_visible(t->rel, t->st, t->snapshot, &rv);
		sift_tbl_free_row(&rv);
		if (vis)
			return true;
	}
	return false;
}

static bool
vblock_tid_visible(void *arg, ItemPointer tid)
{
	VblockTest *t = arg;
	SiftTblRowVer rv;
	const SiftTblRec *rec;

	if (t->copy)
	{
		if ((rec = sift_tbl_page_rec(t->copy, t->gen, ItemPointerGetOffsetNumber(tid))) != NULL)
		{
			sift_tbl_rowver(&rv, tid, rec);
			if (sift_tbl_visible(t->rel, t->st, t->snapshot, &rv))
				return true;
		}
		return chain_visible(t, sift_key_from_tid(tid));
	}
	rv = (SiftTblRowVer) {.key = sift_key_from_tid(tid),.wbuf = false};
	sift_tbl_vers_rowver(&t->vp, ItemPointerGetOffsetNumber(tid), &rv.xmin, &rv.cmin);
	return sift_tbl_visible(t->rel, t->st, t->snapshot, &rv) || chain_visible(t, rv.key);
}

/* Keeps the offsets of vblock whose row is visible to snapshot (chain: or whose HOT chain has a version that is: the TIDs of an index); the write buffer page is read once; a unit the snapshot sees no insertion of is no row at all */
int
sift_tbl_filter_page_vers(Relation rel, Snapshot snapshot, SiftTblVers *vers, BlockNumber vblock, uint64 *offmask, bool chain)
{
	PGAlignedBlock copy;
	VblockTest	t = {.rel = rel,.st = sift_tbl_rel(rel),.snapshot = snapshot,.copy = copy.data,.chain = chain};

	if (!sift_tbl_load_vblock(rel, t.st, vblock, copy.data, &t.gen))
	{
		SiftTblVerState state = sift_tbl_vers_sb(rel, snapshot, vers, vblock / SIFT_SB_PAGES);

		t.copy = NULL;
		if (state == SIFT_TBL_VER_NONE)
		{
			memset(offmask, 0, SIFT_OFFMASK_WORDS * sizeof(uint64));
			return 0;
		}
		if (state == SIFT_TBL_VER_ROWS)
			sift_tbl_vers_page(vers, vblock, &t.vp);
		else if (!chain && snapshot->snapshot_type == SNAPSHOT_MVCC && !IsolationIsSerializable())
		{
			/*
			 * every row of the page was inserted for the snapshot: what is left to ask of a row is its tombstones, read once for the page where a row asks
			 * for them with two bisections of the log (about 300 instructions); a page asked for fewer than PAGE_ROWS_MIN rows, or with more entries than three for each row asked (a key lookup
			 * leaves both), keeps the rows one by one: its bisections cost about a third of them. A serializable snapshot also records the conflict of each row it asks about.
			 */
			int			want = 0;

			for (int wd = 0; wd < SIFT_OFFMASK_WORDS; wd++)
				for (uint64 m = offmask[wd]; m && want < PAGE_ROWS_MIN; m &= m - 1)
					want++;
			if (want >= PAGE_ROWS_MIN)
			{
				uint64		deleted[SIFT_OFFMASK_WORDS] = {0};
				int			left = 0;

				want = 0;
				for (int wd = 0; wd < SIFT_OFFMASK_WORDS; wd++)
					want += sift_pc64(offmask[wd]);
				if (sift_tomb_deleted_offsets(&t.st->tomb, snapshot, vblock, 3 * want, deleted, NULL))
				{
					for (int wd = 0; wd < SIFT_OFFMASK_WORDS; wd++)
					{
						offmask[wd] &= ~deleted[wd];
						left += sift_pc64(offmask[wd]);
					}
					return left;
				}
			}
		}
	}
	return sift_vis_filter_mask(vblock, offmask, vblock_tid_visible, &t);
}

/* The same for a reader that goes by the directory this backend holds (the BM25 index's heap check) */
int
sift_tbl_filter_page(Relation rel, Snapshot snapshot, BlockNumber vblock, uint64 *offmask)
{
	SiftTblRel *st = sift_tbl_rel(rel);

	if (st->curvers == NULL || st->curvers_version != st->dir.version || st->curvers_rel != rel)
	{
		if (st->curvers_cxt)
			MemoryContextDelete(st->curvers_cxt);
		st->curvers_cxt = AllocSetContextCreate(st->cxt, "sift_table versions of the directory", ALLOCSET_SMALL_SIZES);
		st->curvers = sift_tbl_vers_build(rel, st, &st->dir, st->curvers_cxt);
		st->curvers_version = st->dir.version;
		st->curvers_rel = rel;
	}
	return sift_tbl_filter_page_vers(rel, snapshot, st->curvers, vblock, offmask, true);
}

bool
sift_tbl_has_chains(Relation rel)
{
	SiftTblRel *st = sift_tbl_rel(rel);

	sift_tomb_sync(rel, &st->tomb);
	return st->tomb.hotm != NULL;
}

void
sift_tbl_chain_members(Relation rel, uint32 *pos, BlockNumber blk, const uint64 *offmask, SiftTblKeyFn fn, void *arg)
{
	SiftTblRel *st = sift_tbl_rel(rel);
	uint64		roots[SIFT_OFFMASK_WORDS] = {0};

	sift_tomb_hot_offsets(&st->tomb, blk, pos, roots);
	for (int wd = 0; wd < SIFT_OFFMASK_WORDS; wd++)
		for (uint64 m = roots[wd] & offmask[wd]; m; m &= m - 1)
		{
			uint64		key = ((uint64) blk << 16) | (wd * 64 + pg_rightmost_one_pos64(m) + 1),
						succ;

			while (sift_tbl_hot_next(st, key, &succ))
			{
				fn(arg, succ);
				key = succ;
			}
		}
}

bool
sift_tbl_tuple_fetch_row_version(Relation rel, ItemPointer tid, Snapshot snapshot, TupleTableSlot *slot)
{
	SiftTblRel *st = sift_tbl_rel(rel);

	sift_tbl_refresh(rel, st);
	sift_tomb_sync(rel, &st->tomb);
	return fetch_visible(rel, st, tid, snapshot, slot, NULL, true, false);
}

bool
sift_tbl_tuple_tid_valid(TableScanDesc scan, ItemPointer tid)
{
	return ItemPointerGetOffsetNumberNoCheck(tid) >= 1 && ItemPointerGetOffsetNumberNoCheck(tid) <= SIFT_TBL_MAX_OFF &&
		ItemPointerGetBlockNumberNoCheck(tid) < sift_tbl_nvblocks(scan->rs_rd);
}

void
sift_tbl_tuple_get_latest_tid(TableScanDesc scan, ItemPointer tid)
{
	Relation	rel = scan->rs_rd;
	SiftTblRel *st = sift_tbl_rel(rel);
	SiftTblRowVer rv;

	sift_tbl_refresh(rel, st);
	sift_tomb_sync(rel, &st->tomb);
	for (;;)
	{
		const SiftTomb *e;
		int			n;
		uint64		succ = 0;

		if (!sift_tbl_fetch_lazy(rel, st, tid, &rv))
			return;
		sift_tbl_free_row(&rv);
		if ((e = sift_tomb_find(&st->tomb, rv.key, &n)) != NULL)
			for (int i = 0; i < n; i++)
				if (e[i].kind == SIFT_TOMB_UPDATE &&
					(TransactionIdIsCurrentTransactionId(e[i].xid) ||
					 (!XidInMVCCSnapshot(e[i].xid, scan->rs_snapshot) && TransactionIdDidCommit(e[i].xid))))
					succ = e[i].succ;
		if (succ == 0)
			return;
		sift_key_to_tid(succ, tid);
	}
}

bool
sift_tbl_tuple_satisfies_snapshot(Relation rel, TupleTableSlot *slot, Snapshot snapshot)
{
	SiftTblRel *st = sift_tbl_rel(rel);
	SiftTblRowVer rv;
	bool		ok;

	sift_tbl_refresh(rel, st);
	sift_tomb_sync(rel, &st->tomb);
	if (!sift_tbl_fetch_lazy(rel, st, &slot->tts_tid, &rv))
		return false;
	ok = sift_tbl_visible(rel, st, snapshot, &rv);
	sift_tbl_free_row(&rv);
	return ok;
}

/* Is the chain that begins at the version del tells of dead for everyone: its end deleted or updated (not HOT) by a removable xid; *xid: the latest of them */
static bool
chain_dead(Relation rel, const SiftTblRel *st, const SiftTblDel *first, TransactionId *xid)
{
	SiftTblDel	d = *first,
			   *del = &d;

	*xid = InvalidTransactionId;
	for (;;)
	{
		if ((del->state != SIFT_TBL_DEL_DELETED && del->state != SIFT_TBL_DEL_UPDATED) || !sift_tbl_xid_removable(rel, del->xid))
			return false;
		if (!TransactionIdIsValid(*xid) || TransactionIdFollows(del->xid, *xid))
			*xid = del->xid;
		if (!del->hot)
			return true;
		sift_tbl_del_state(st, del->succ, del);
	}
}

static int
deltid_cmp(const void *a, const void *b)
{
	return ItemPointerCompare((ItemPointer) &((const TM_IndexDelete *) a)->tid, (ItemPointer) &((const TM_IndexDelete *) b)->tid);
}

/*
 * TIDs no snapshot will see any more: those whose deleter is removable now, by the tombstone log (a TID is never reused, so the log of its
 * key is the row's whole history), and in a simple deletion those whose row is gone, which VACUUM removed with its tombstone. A row is read
 * for the second only: a bottom-up pass, which the index runs before it splits a page of versions that UPDATEs left behind, is answered by
 * the log alone, where it read every row of the page (several hundred, each from another superblock of the column groups). The rows of a
 * simple deletion are read in TID order, so that those of one superblock meet the fetch scan that caches it.
 */
TransactionId
sift_tbl_index_delete_tuples(Relation rel, TM_IndexDeleteOp *delstate)
{
	SiftTblRel *st = sift_tbl_rel(rel);
	TransactionId horizon = InvalidTransactionId;
	int			final = 0,
				freed = 0;

	sift_tbl_refresh(rel, st);
	sift_tomb_sync(rel, &st->tomb);
	if (!delstate->bottomup)
		qsort(delstate->deltids, delstate->ndeltids, sizeof(TM_IndexDelete), deltid_cmp);
	for (int i = 0; i < delstate->ndeltids; i++)
	{
		TM_IndexDelete *d = &delstate->deltids[i];
		TM_IndexStatus *status = &delstate->status[d->id];
		SiftTblRowVer rv;
		SiftTblDel	del;
		TransactionId xid;

		if (delstate->bottomup && freed >= delstate->bottomupfreespace)
			break;
		sift_tbl_del_state(st, sift_key_from_tid(&d->tid), &del);
		if (chain_dead(rel, st, &del, &xid))
		{
			status->knowndeletable = true;
			if (!TransactionIdIsValid(horizon) || TransactionIdFollows(xid, horizon))
				horizon = xid;
		}
		else if (!delstate->bottomup && del.state == SIFT_TBL_DEL_NONE)
		{
			if (!sift_tbl_fetch_lazy(rel, st, &d->tid, &rv))
				status->knowndeletable = true;
			else
				sift_tbl_free_row(&rv);
		}
		if (status->knowndeletable)
		{
			final = i + 1;
			freed += status->freespace;
		}
	}
	/* the array ends at its last deletable entry (none left: a bottom-up pass found nothing) */
	delstate->ndeltids = final;
	return horizon;
}

#ifdef SIFT_TESTING
bool		sift_test_bitmap_no_visible = false;
#define BITMAP_NO_VISIBLE	sift_test_bitmap_no_visible
#else
#define BITMAP_NO_VISIBLE	false
#endif

static void
bitmap_add(SiftTblScan *s, HeapTuple tup)
{
	if (s->nbrows == s->nbcap)
	{
		s->nbcap = Max(64, s->nbcap * 2);
		SIFT_REGROW_IN(GetMemoryChunkContext(s), s->brows, s->nbcap * sizeof(HeapTuple));
	}
	s->brows[s->nbrows++] = tup;
}

/*
 * The visible version of the chain that begins at the row of tid (its entries lead there; the row may be gone): a HOT update's new version is no
 * entry of any index and no page scan returns it, so the bitmap reaches it from the root. emitted: the offsets of the page whose own row was added.
 */
static void
bitmap_chains(Relation rel, SiftTblScan *s, BlockNumber vblock, const OffsetNumber *offsets, int n, const uint64 *emitted)
{
	uint64		roots[SIFT_OFFMASK_WORDS] = {0};
	int			cnt = n;

	if (n < 0)
	{
		sift_tomb_hot_offsets(&s->st->tomb, vblock, NULL, roots);
		cnt = SIFT_TBL_MAX_OFF;
	}
	for (int i = 0; i < cnt; i++)
	{
		OffsetNumber off = n < 0 ? i + 1 : offsets[i];
		uint64		key = ((uint64) vblock << 16) | off;
		uint64		succ;

		if ((n < 0 && !sift_col_bit_test(roots, off - 1)) || sift_col_bit_test(emitted, off - 1) || sift_tomb_hot_member(&s->st->tomb, key))
			continue;
		while (sift_tbl_hot_next(s->st, key, &succ))
		{
			SiftTblRowVer rv;
			ItemPointerData tid;

			sift_key_to_tid(succ, &tid);
			key = succ;
			if (!sift_tbl_fetch_row(rel, s->st, &tid, &rv))
				continue;
			if (sift_tbl_visible(rel, s->st, s->base.rs_snapshot, &rv))
			{
				bitmap_add(s, rv.tuple);
				break;
			}
			sift_tbl_free_row(&rv);
		}
	}
}

/*
 * A bitmap's rows of the column groups are not decoded when the page is read: they wait, and the slot decodes the columns the plan reads when it reads them. Two ways to wait:
 * the place of each row in the unit of g, the scan's group scan (one pass over the page: the superblock's TIDs are read once for its pages, and the visibility of a row is tested from
 * the versions of the unit), or the TID alone, fetched as an index scan fetches it (fetch_visible: the units that the statements before kept decoded cost nothing; each row is found
 * by its TID: 10,000 instructions a row against 2,900 for a row of g, which pays 0.8 to 2.4 M instructions to read the TIDs of the superblock once). Records of eight statements on 300,000 and
 * 1,000,000 rows, the rows of a superblock from which g is used swept from 0 to never: 50 is within 5% of the best way of each statement on average and 23% at worst; 25 pays the superblock for
 * 25 rows (3 times the best), 400 costs 2 times the best in the middle (150 parents of 300,000 rows: 92 M instructions against 49 M)
 */
#define BITMAP_BULK_ROWS	50		/* and the default of sift.test_bitmap_bulk_rows (core/handler_test.c) */

#ifdef SIFT_TESTING
int			sift_test_bitmap_bulk_rows = BITMAP_BULK_ROWS;
#define BULK_ROWS	sift_test_bitmap_bulk_rows
#else
#define BULK_ROWS	BITMAP_BULK_ROWS
#endif

static void
bitmap_lazy_page(Relation rel, SiftTblScan *s, BlockNumber vblock, const OffsetNumber *offsets, int n)
{
	uint64		mask[SIFT_OFFMASK_WORDS] = {0};
	uint64		emitted[SIFT_OFFMASK_WORDS] = {0};
	int			cnt;

	if (s->lrows == NULL)
	{
		s->lrows = MemoryContextAlloc(GetMemoryChunkContext(s), SIFT_TBL_MAX_OFF * sizeof(SiftTblBlockRow));
		s->ltids = MemoryContextAlloc(GetMemoryChunkContext(s), SIFT_TBL_MAX_OFF * sizeof(ItemPointerData));
	}
	if (vblock / SIFT_SB_PAGES != s->lsb)
	{
		s->lsb = vblock / SIFT_SB_PAGES;
		s->lsb_rows = 0;
	}
	if (n >= 0)
		s->lsb_rows += n;
	if (n >= 0 && s->lsb_rows < BULK_ROWS)
	{
		for (int i = 0; i < n; i++)
			ItemPointerSet(&s->ltids[s->nltids++], vblock, offsets[i]);
		return;
	}
	if (s->g == NULL)
		s->g = scan_gscan(s, s->base.rs_snapshot);
	for (int i = 0; i < n; i++)
		sift_col_bit_set(mask, offsets[i] - 1);
	cnt = sift_tbl_gscan_block(s->g, vblock, n < 0 ? NULL : mask, NULL, NULL, NULL, s->lrows);
	for (int i = 0; i < cnt; i++)
	{
		SiftTblRowVer rv = {.key = sift_key_from_tid(&s->lrows[i].tid),.wbuf = false,.xmin = s->lrows[i].xmin,.cmin = s->lrows[i].cmin};

		if (!sift_tomb_hot_member(&s->st->tomb, rv.key) && (BITMAP_NO_VISIBLE || sift_tbl_visible(rel, s->st, s->base.rs_snapshot, &rv)))
		{
			s->lrows[s->nlrows++] = s->lrows[i];
			sift_col_bit_set(emitted, ItemPointerGetOffsetNumberNoCheck(&s->lrows[i].tid) - 1);
		}
	}
	bitmap_chains(rel, s, vblock, offsets, n, emitted);
}

/* The visible rows of vblock of the column groups at the given offsets (n < 0: all), decoded: the sample's */
static void
bitmap_group_page(Relation rel, SiftTblScan *s, BlockNumber vblock, const OffsetNumber *offsets, int n, bool chain)
{
	uint64		mask[SIFT_OFFMASK_WORDS] = {0};
	HeapTuple	row[SIFT_TBL_MAX_OFF];
	TransactionId xmin[SIFT_TBL_MAX_OFF];
	CommandId	cmin[SIFT_TBL_MAX_OFF];
	int			cnt;

	if (vblock >= SIFT_TBL_NEXT_VBLOCK(&s->dir->meta))
		return;
	if (chain)
	{
		bitmap_lazy_page(rel, s, vblock, offsets, n);
		return;
	}
	if (s->g == NULL)
		s->g = scan_gscan(s, s->base.rs_snapshot);
	for (int i = 0; i < n; i++)
		sift_col_bit_set(mask, offsets[i] - 1);
	cnt = sift_tbl_gscan_block(s->g, vblock, n < 0 ? NULL : mask, row, xmin, cmin, NULL);
	for (int i = 0; i < cnt; i++)
	{
		SiftTblRowVer rv = {.key = sift_key_from_tid(&row[i]->t_self),.wbuf = false,.xmin = xmin[i],.cmin = cmin[i]};

		if (sift_tbl_visible(rel, s->st, s->base.rs_snapshot, &rv))
			bitmap_add(s, row[i]);
		else
			heap_freetuple(row[i]);
	}
}

/* Collects the visible rows of vblock at the given offsets (n < 0: every slot) */
static void
bitmap_page(Relation rel, SiftTblScan *s, BlockNumber vblock, const OffsetNumber *offsets, int n, bool chain)
{
	uint64		gen;
	bool		held;
	int			cnt = n < 0 ? SIFT_TBL_MAX_OFF : n;
	uint64		emitted[SIFT_OFFMASK_WORDS] = {0};

	s->nbrows = s->bri = 0;
	s->nlrows = s->lri = s->nltids = s->lti = 0;
	/* the write buffers the scan began with, like its column groups (s->dir): a UPDATE reads the directory again at every row */
	if (!sift_tbl_load_vblock_in(rel, s->seg, s->nseg, vblock, s->copy, &gen, &held))
	{
		bitmap_group_page(rel, s, vblock, offsets, n, chain);
		return;
	}
	for (int i = 0; i < cnt; i++)
	{
		OffsetNumber off = n < 0 ? i + 1 : offsets[i];
		const SiftTblRec *rec = sift_tbl_page_rec(s->copy, gen, off);
		ItemPointerData tid;
		SiftTblRowVer rv;

		if (rec == NULL)
			continue;
		ItemPointerSet(&tid, vblock, off);
		sift_tbl_rowver(&rv, &tid, rec);
		if ((chain && sift_tomb_hot_member(&s->st->tomb, rv.key)) || !sift_tbl_visible(rel, s->st, s->base.rs_snapshot, &rv))
			continue;
		sift_tbl_rowver_tuple(rel, &rv, rec, gen, &tid);
		bitmap_add(s, rv.tuple);
		sift_col_bit_set(emitted, off - 1);
	}
	if (chain)
		bitmap_chains(rel, s, vblock, offsets, n, emitted);
}

static bool
bitmap_emit(Relation rel, SiftTblScan *s, TupleTableSlot *slot)
{
	if (s->bri < s->nbrows)
	{
		sift_tbl_store(slot, s->brows[s->bri++]);
		return true;
	}
	while (s->lri < s->nlrows)
	{
		const SiftTblBlockRow *r = &s->lrows[s->lri++];
		SiftTblRowVer rv;

		if (sift_tbl_slot_is_lazy(slot))
			sift_tbl_slot_store_group(slot, rel, (ItemPointer) &r->tid, s->g, r->token, r->pos);
		else if (sift_tbl_fetch_row(rel, s->st, (ItemPointer) &r->tid, &rv))
			sift_tbl_store(slot, rv.tuple);
		else
			continue;
		return true;
	}
	while (s->lti < s->nltids)
		if (fetch_visible(rel, s->st, &s->ltids[s->lti++], BITMAP_NO_VISIBLE ? SnapshotAny : s->base.rs_snapshot, slot, NULL, false, true))
			return true;
	return false;
}

static int
cmp_offset(const void *a, const void *b)
{
	return pg_cmp_u16(ItemPointerGetOffsetNumberNoCheck(&(*(const HeapTuple *) a)->t_self),
					  ItemPointerGetOffsetNumberNoCheck(&(*(const HeapTuple *) b)->t_self));
}

/* The vblocks stand for the blocks: the method (SYSTEM) picks one, or they come in order (BERNOULLI); its visible rows are read at once */
bool
sift_tbl_scan_sample_next_block(TableScanDesc scan, SampleScanState *scanstate)
{
	SiftTblScan *s = (SiftTblScan *) scan;
	TsmRoutine *tsm = scanstate->tsmroutine;
	BlockNumber nblocks = sift_tbl_nvblocks(scan->rs_rd);
	MemoryContext old;

	if (tsm->NextSampleBlock)
		s->sample_blk = tsm->NextSampleBlock(scanstate, nblocks);
	else
		s->sample_blk = s->sample_blk == InvalidBlockNumber ? 0 : s->sample_blk + 1;
	if (s->sample_blk >= nblocks)
		s->sample_blk = InvalidBlockNumber;
	if (s->sample_blk == InvalidBlockNumber)
		return false;
	MemoryContextReset(s->rowcxt);
	old = MemoryContextSwitchTo(s->rowcxt);
	bitmap_page(scan->rs_rd, s, s->sample_blk, NULL, -1, false);
	MemoryContextSwitchTo(old);
	qsort(s->brows, s->nbrows, sizeof(HeapTuple), cmp_offset);
	s->sample_max = s->nbrows > 0 ? ItemPointerGetOffsetNumberNoCheck(&s->brows[s->nbrows - 1]->t_self) : 0;
	return true;
}

bool
sift_tbl_scan_sample_next_tuple(TableScanDesc scan, SampleScanState *scanstate, TupleTableSlot *slot)
{
	SiftTblScan *s = (SiftTblScan *) scan;
	OffsetNumber off;

	/* (OffsetNumberIsValid evaluates its argument twice) */
	while ((off = scanstate->tsmroutine->NextSampleTuple(scanstate, s->sample_blk, s->sample_max)) != InvalidOffsetNumber)
	{
		CHECK_FOR_INTERRUPTS();
		while (s->bri < s->nbrows && ItemPointerGetOffsetNumberNoCheck(&s->brows[s->bri]->t_self) < off)
			s->bri++;
		if (s->bri < s->nbrows && ItemPointerGetOffsetNumberNoCheck(&s->brows[s->bri]->t_self) == off)
		{
			sift_tbl_slot_store_tuple(slot, s->brows[s->bri++]);
			return true;
		}
	}
	return false;
}

#if PG_VERSION_NUM >= 180000

bool
sift_tbl_scan_bitmap_next_tuple(TableScanDesc scan, TupleTableSlot *slot, bool *recheck, uint64 *lossy_pages,
								uint64 *exact_pages)
{
	SiftTblScan *s = (SiftTblScan *) scan;

	for (;;)
	{
		TBMIterateResult tbmres;

		if (bitmap_emit(scan->rs_rd, s, slot))
			return true;
		if (!tbm_iterate(&scan->st.rs_tbmiterator, &tbmres))
			return false;
		CHECK_FOR_INTERRUPTS();
		if (tbmres.lossy)
		{
			bitmap_page(scan->rs_rd, s, tbmres.blockno, NULL, -1, true);
			(*lossy_pages)++;
		}
		else
		{
			OffsetNumber offsets[MaxHeapTuplesPerPage];
			int			n = tbm_extract_page_tuple(&tbmres, offsets, lengthof(offsets));

			bitmap_page(scan->rs_rd, s, tbmres.blockno, offsets, n, true);
			(*exact_pages)++;
		}
		*recheck = tbmres.recheck;
	}
}

#else

bool
sift_tbl_scan_bitmap_next_block(TableScanDesc scan, TBMIterateResult *tbmres)
{
	SiftTblScan *s = (SiftTblScan *) scan;

	CHECK_FOR_INTERRUPTS();
	bitmap_page(scan->rs_rd, s, tbmres->blockno, tbmres->ntuples < 0 ? NULL : tbmres->offsets, tbmres->ntuples, true);
	return s->nbrows > 0;
}

bool
sift_tbl_scan_bitmap_next_tuple(TableScanDesc scan, TBMIterateResult *tbmres, TupleTableSlot *slot)
{
	return bitmap_emit(scan->rs_rd, (SiftTblScan *) scan, slot);
}

#endif
