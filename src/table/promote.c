/*
 * table/promote.c - promotion of the sealed write buffers into column segments, and what a closing writer settles
 */
#include "postgres.h"

#include "access/genam.h"
#include "access/heapam.h"
#include "access/table.h"
#include "access/htup_details.h"
#include "access/tupmacs.h"
#include "access/xact.h"
#include "catalog/namespace.h"
#include "catalog/objectaddress.h"
#include "catalog/pg_class.h"
#include "utils/acl.h"
#include "utils/fmgroids.h"
#include "utils/inval.h"
#include "catalog/pg_type.h"
#include "funcapi.h"
#include "miscadmin.h"
#include "access/skey.h"
#include "port/pg_bitutils.h"
#include "storage/bufmgr.h"
#include "storage/procarray.h"
#include "utils/builtins.h"
#include "utils/datum.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/numeric.h"
#include "utils/rel.h"
#include "utils/snapmgr.h"
#include "utils/timestamp.h"
#include "utils/tuplestore.h"

#include "col/am.h"
#include "col/pg.h"
#include "col/seg.h"
#include "core/lane.h"
#include "bm25/maint.h"
#include "bm25/merge.h"
#include "core/ops.h"
#include "table/table.h"
#include "table/seg_int.h"

typedef enum Fate
{
	FATE_KEEP,
	FATE_DROP,
	FATE_WAIT
} Fate;

/* A row goes into the group when its insertion is decided and every snapshot sees it; VACUUM drops the rows it flagged DEAD */
static Fate
row_fate(const SiftTblRec *rec, TransactionId oldest, bool vacuum)
{
	if (rec->flags & SIFT_TBL_REC_DEAD)
		return vacuum ? FATE_DROP : FATE_WAIT;
	if (!TransactionIdIsNormal(rec->xmin))
		return FATE_KEEP;
	return TransactionIdPrecedes(rec->xmin, oldest) && !sift_tbl_xid_aborted(rec->xmin) ? FATE_KEEP : FATE_WAIT;
}

/* What a sealed write buffer can become */
typedef enum SegClass
{
	SEG_WAIT,					/* some row is not final and the segment cannot carry it (nor is the directory filling) */
	SEG_FINAL,					/* every row is final: a frozen group */
	SEG_UNFROZEN				/* some row is not: the group keeps the xmin and cmin of every row that is not final */
} SegClass;

/* The fate of the rows of one writer, looked up once for the run of records that share it */
typedef struct FateMemo
{
	TransactionId xmin;
	Fate		fate;
	bool		aborted;
	bool		valid;
} FateMemo;

static Fate
fate_memo(FateMemo *m, const SiftTblRec *rec, TransactionId oldest, bool vacuum)
{
	if ((rec->flags & SIFT_TBL_REC_DEAD) || !m->valid || m->xmin != rec->xmin)
	{
		m->fate = row_fate(rec, oldest, vacuum);
		m->aborted = TransactionIdIsNormal(rec->xmin) && m->fate == FATE_WAIT && sift_tbl_xid_aborted(rec->xmin);
		m->xmin = rec->xmin;
		m->valid = !(rec->flags & SIFT_TBL_REC_DEAD);
	}
	return m->fate;
}

/*
 * What the rows of the segment allow. A segment whose rows are not all final becomes a group with unfrozen rows only when pressure says the
 * directory is filling, whatever the mix of writers, commands and subtransactions is; a row VACUUM flagged DEAD (it drops them) or a speculative
 * insertion in progress keeps it waiting. The newest rows are in the last pages, so a segment that cannot be promoted answers after one page.
 */
static SegClass
seg_class(Relation rel, SiftTblSeg *seg, TransactionId oldest, bool vacuum, bool pressure)
{
	PGAlignedBlock copy;
	FateMemo	memo = {0};
	bool		unfrozen = false;

	for (uint32 p = seg->npages; p-- > 0;)
	{
		if (!seg_page(rel, seg, p, copy.data))
			continue;
		for (uint32 off = 1; off <= TBL_PAGE(copy.data)->nrows; off++)
		{
			const SiftTblRec *rec = sift_tbl_page_rec(copy.data, seg->gen, off);

			if (rec == NULL || fate_memo(&memo, rec, oldest, vacuum) != FATE_WAIT)
				continue;
			if (!pressure || (rec->flags & (SIFT_TBL_REC_DEAD | SIFT_TBL_REC_SPEC)))
				return SEG_WAIT;
			unfrozen = true;
		}
	}
	return unfrozen ? SEG_UNFROZEN : SEG_FINAL;
}

void
seal_flush(SealWriter *sw)
{
	MemoryContext old = MemoryContextSwitchTo(sw->cxt);

	if (sw->batch->nrows > 0)
	{
		if (sw->w == NULL)
		{
			sw->wt = sift_wt_begin(sw->rel, SIFT_WRITE_SHARED);
			sift_wt_expect(sw->wt, sw->expect);
			sw->w = sift_colw_begin(sw->wt, sift_store_new_gen(sw->rel), sw->l->schema, sw->policy, sw->format, sw->cxt);
		}
		sift_colw_add(sw->w, sw->batch);
	}
	MemoryContextSwitchTo(old);
}

/*
 * The rows of the segment e as one column segment with the same TIDs: a row that is final is frozen, the others keep their xmin and cmin
 * (the group then has unfrozen rows); the write buffer pages are released with the switch
 */
static void
seal_segment(Relation rel, SiftTblRel *st, const SiftSegEntry *e, SiftTblSeg *seg, TransactionId oldest, bool vacuum, bool vers)
{
	const SiftTblLayout *l = layout_of(rel, st);
	TupleDesc	desc = RelationGetDescr(rel);
	MemoryContext cx = AllocSetContextCreate(CurrentMemoryContext, "sift_table promote", ALLOCSET_DEFAULT_SIZES);
	MemoryContext rowcx = AllocSetContextCreate(cx, "sift_table promote row", ALLOCSET_SMALL_SIZES);
	MemoryContext old = MemoryContextSwitchTo(cx);
	SealWriter	sw = {rel, l, layout_policy(rel, l), cx, sift_cbatch_create(cx, l->schema), NULL, NULL, (uint64) seg->npages * BLCKSZ, tbl_col_format(vers)};
	FateMemo	memo = {0};
	PGAlignedBlock copy;
	Datum	   *values = palloc(desc->natts * sizeof(Datum));
	bool	   *isnull = palloc(desc->natts * sizeof(bool));
	SiftSegEntry neu;
	SiftMaintSwitch sw_dir = {.old = &e->gen,.nold = 1};
	SiftExtent *ext;
	int			next;
	uint32		sb = PG_UINT32_MAX;

	for (uint32 p = 0; p < seg->npages; p++)
	{
		BlockNumber vblock = seg->vbase + p;

		if (!seg_page(rel, seg, p, copy.data))
			continue;
		for (uint32 off = 1; off <= TBL_PAGE(copy.data)->nrows; off++)
		{
			const SiftTblRec *rec = sift_tbl_page_rec(copy.data, seg->gen, off);
			ItemPointerData tid;
			SiftColRow	row;
			HeapTupleData tup;
			Fate		fate;

			if (rec == NULL || (fate = fate_memo(&memo, rec, oldest, vacuum)) == FATE_DROP)
				continue;
			CHECK_FOR_INTERRUPTS();
			MemoryContextSwitchTo(rowcx);
			ItemPointerSet(&tid, vblock, off);
			tup = *sift_tbl_rec_tuple(rel, rec, seg->gen, sift_key_from_tid(&tid));
			heap_deform_tuple(&tup, desc, values, isnull);
			row_in(l, desc, &tid, values, isnull, &row);
			if (vblock / SIFT_SB_PAGES != sb)
			{
				seal_flush(&sw);
				sb = vblock / SIFT_SB_PAGES;
				sift_cbatch_reset(sw.batch, sb);
			}
			sift_cbatch_add_ver(sw.batch, &row, fate == FATE_KEEP ? InvalidTransactionId : rec->xmin, rec->cmin);
			MemoryContextSwitchTo(cx);
			MemoryContextReset(rowcx);
		}
	}
	seal_flush(&sw);
	if (sw.w)
	{
		sift_colw_finish(sw.w, &neu);
		sift_wt_finish(sw.wt);
		sw_dir.neu = &neu;
	}
	ext = sift_lane_extents(rel, e, &next);
	sift_dir_update(rel, &(SiftDirChange) {.fn = sift_maint_switch_segments,.arg = &sw_dir,.release = ext,.nrelease = next,
			.extra = InvalidBuffer,.immediate = st->fresh});
	MemoryContextSwitchTo(old);
	MemoryContextDelete(cx);
}

static int
promote_sealed(Relation rel, SiftTblRel *st, TransactionId oldest, bool vacuum)
{
	int			promoted = 0;
	SiftDirSnapshot *dir;
	SegClass	class;
	bool		pressure;

	sift_tbl_refresh(rel, st);
	pressure = dir_pressure(st);
	dir = MemoryContextAlloc(CurrentMemoryContext, sizeof(SiftDirSnapshot));
	*dir = st->dir;
	for (int i = 0; i < dir->nsegs; i++)
	{
		const SiftSegEntry *e = &dir->segs[i];
		SiftTblSeg seg = {.gen = e->gen,.hdr = e->header_blk,.vbase = (uint64) e->sb_lo * SIFT_TBL_VBLOCKS_SB};

		if (e->kind != SIFT_SEG_WBUF_SEALED || !(e->flags & SIFT_TBL_SEG_WBUF))
			continue;
		/* the writer that closed the segment flags its pages after it swapped it in the directory: until every page is flagged a session that holds one still appends */
		sift_lane_flag_sealed_pages(rel, e);
		sift_tbl_seg_load(rel, &seg);
		class = seg_class(rel, &seg, oldest, vacuum, pressure);
		if (class == SEG_WAIT)
			continue;
		seal_segment(rel, st, e, &seg, oldest, vacuum, class == SEG_UNFROZEN);
		promoted++;
	}
	pfree(dir);
	if (promoted > 0)
		sift_tbl_refresh(rel, st);
	return promoted;
}

int
sift_tbl_promote(Relation rel, SiftTblRel *st, TransactionId oldest, bool vacuum)
{
	int			promoted;

	if (!sift_maint_lock(rel, vacuum))
		return 0;
	promoted = promote_sealed(rel, st, oldest, vacuum);
	sift_maint_unlock(rel);
	return promoted;
}

/*
 * A writer closed a write buffer: promote the segments whose rows have settled, then merge the column segments (at most steps merges)
 * while the directory is more than half full. Only VACUUM merged, so a load in many transactions left one small column segment per closed
 * write buffer and ran out of the directory entries (SIFT_MAX_SEGMENTS) long before autovacuum came. A load in one transaction cannot
 * settle, and from half the directory on its write buffers become tagged groups (seg_class) that merge like the others, so it does not run
 * out of them either.
 * Everything here is under the maintenance lock, taken only if it is free (wait: an insert that finds the directory full waits for it, as for
 * a worker that is promoting what it needs): the steps are VACUUM's too. Returns the write buffers promoted.
 */
int
sift_tbl_settle(Relation rel, SiftTblRel *st, TransactionId oldest, bool wait, int steps)
{
	int			promoted;

	if (!sift_maint_lock(rel, wait))
		return 0;
	promoted = promote_sealed(rel, st, oldest, false);
	(void) sift_tbl_freeze_groups(rel, st, oldest);
	for (int i = 0; i < steps && dir_pressure(st) && sift_tbl_merge(rel, st, oldest); i++)
		CHECK_FOR_INTERRUPTS();
	sift_maint_unlock(rel);
	return promoted;
}
