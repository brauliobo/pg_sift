/*
 * table/bulk.c - the new table of a rewrite (VACUUM FULL, CLUSTER) written as column segments
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

/* ---------- a rewrite (VACUUM FULL, CLUSTER): the new table written as column segments, without row-format pages ---------- */

struct SiftTblBulk
{
	SealWriter	sw;
	SiftTblRel *st;
	TupleDesc	desc;
	MemoryContext rowcx;
	Datum	   *values;
	bool	   *isnull;
	uint64		vbase;			/* row i has the TID (vbase + i / SIFT_TBL_MAX_OFF, i % SIFT_TBL_MAX_OFF + 1) */
	uint64		nrows;
	Size		budget;			/* memory a segment's writer may hold before the next one starts */
	SiftSegEntry segs[SIFT_MAX_SEGMENTS];
	int			nsegs;
};

typedef struct BulkPublish
{
	const SiftSegEntry *segs;
	int			n;
	uint64		next_vblock;
} BulkPublish;

static void
bulk_publish(SiftMetaPageData *meta, SiftDirectoryData *dir, Page extra, void *arg)
{
	BulkPublish *p = arg;

	for (int i = 0; i < p->n; i++)
	{
		dir->segs[dir->nsegs++] = p->segs[i];
		meta->ndocs += p->segs[i].ndocs;
	}
	SIFT_TBL_NEXT_VBLOCK(meta) = Max(SIFT_TBL_NEXT_VBLOCK(meta), p->next_vblock);
}

SiftTblBulk *
sift_tbl_bulk_begin(Relation rel, SiftTblRel *st)
{
	const SiftTblLayout *l = layout_of(rel, st);
	MemoryContext cx = AllocSetContextCreate(CurrentMemoryContext, "sift_table rewrite", ALLOCSET_DEFAULT_SIZES);
	MemoryContext old = MemoryContextSwitchTo(cx);
	SiftTblBulk *b = palloc0(sizeof(SiftTblBulk));

	b->sw = (SealWriter) {rel, l, layout_policy(rel, l), cx, sift_cbatch_create(cx, l->schema), NULL, NULL, 0, tbl_col_format(false)};
	b->st = st;
	b->desc = RelationGetDescr(rel);
	b->rowcx = AllocSetContextCreate(cx, "sift_table rewrite row", ALLOCSET_SMALL_SIZES);
	b->values = palloc(b->desc->natts * sizeof(Datum));
	b->isnull = palloc(b->desc->natts * sizeof(bool));
	b->budget = Max((Size) maintenance_work_mem * 1024 / 2, 64 * 1024 * 1024);
	sift_tbl_refresh(rel, st);
	b->vbase = TYPEALIGN(SIFT_TBL_VBLOCKS_SB, SIFT_TBL_NEXT_VBLOCK(&st->dir.meta));
	sift_cbatch_reset(b->sw.batch, b->vbase / SIFT_SB_PAGES);
	MemoryContextSwitchTo(old);
	return b;
}

static void
bulk_end_segment(SiftTblBulk *b)
{
	MemoryContext old = MemoryContextSwitchTo(b->sw.cxt);

	sift_colw_finish(b->sw.w, &b->segs[b->nsegs++]);
	sift_wt_finish(b->sw.wt);
	b->sw.w = NULL;
	MemoryContextSwitchTo(old);
}

/* Writes the batch as a unit; a segment whose writer passed the budget ends (while the directory has room for more) */
static void
bulk_flush(SiftTblBulk *b)
{
	seal_flush(&b->sw);
	if (b->sw.w && sift_colw_memory(b->sw.w) > b->budget && b->nsegs < SIFT_MAX_SEGMENTS - 8)
		bulk_end_segment(b);
}

/* The next dense TID; the batch moves to its superblock */
static void
bulk_next_tid(SiftTblBulk *b, ItemPointer tid)
{
	uint64		vblock = b->vbase + b->nrows / SIFT_TBL_MAX_OFF;

	ItemPointerSet(tid, vblock, b->nrows % SIFT_TBL_MAX_OFF + 1);
	if (vblock / SIFT_SB_PAGES != b->sw.batch->sb)
	{
		bulk_flush(b);
		sift_cbatch_reset(b->sw.batch, vblock / SIFT_SB_PAGES);
	}
	b->nrows++;
}

/* Adds a row of the table's layout (its strings and images are copied) under its new TID */
static void
bulk_row(SiftTblBulk *b, SiftColRow *row)
{
	bulk_next_tid(b, &row->tid);
	sift_cbatch_add(b->sw.batch, row);
}

void
sift_tbl_bulk_tuple(SiftTblBulk *b, HeapTuple tup)
{
	MemoryContext old = MemoryContextSwitchTo(b->rowcx);
	SiftColRow	row;

	heap_deform_tuple(tup, b->desc, b->values, b->isnull);
	row_in(b->sw.l, b->desc, &tup->t_self, b->values, b->isnull, &row);
	bulk_row(b, &row);
	MemoryContextSwitchTo(old);
	MemoryContextReset(b->rowcx);
}

/*
 * Are all column segments of the old table written for the new table's layout (their units are copied as they decode), and keep the kinds of
 * their path leaves (a copy of a unit that does not writes its JSON nulls as missing keys: the rows are formed from the tuples instead, which
 * derives the leaves from the documents again)?
 */
static bool
bulk_same_layout(SiftTblBulk *b, Relation old, SiftTblRel *ost)
{
	for (int i = 0; i < ost->dir.nsegs; i++)
	{
		SiftColSegment *seg;
		bool		exact;

		if (ost->dir.segs[i].kind != SIFT_SEG_IMMUTABLE)
			continue;
		if (schema_of(old, ost, &ost->dir.segs[i])->hash != b->sw.l->schema->hash)
			return false;
		if (!b->sw.l->schema->kinds || ost->dir.segs[i].format >= SIFT_COL_FORMAT_KINDS)
			continue;
		seg = sift_cseg_open(old, &ost->dir.segs[i], b->sw.cxt);
		exact = sift_cseg_kinds_exact(seg);
		sift_cseg_close(seg);
		if (!exact)
			return false;
	}
	return true;
}

static int
cmp_sb_lo(const void *a, const void *b)
{
	return pg_cmp_u32(((const SiftSegEntry *) a)->sb_lo, ((const SiftSegEntry *) b)->sb_lo);
}

/* The live rows of the units of segment e that keep accepts, decoded and re-keyed: no tuple is formed */
static void
bulk_segment(SiftTblBulk *b, Relation old, const SiftSegEntry *e, SiftTblKeep keep, void *arg)
{
	MemoryContext cx = AllocSetContextCreate(CurrentMemoryContext, "sift_table rewrite segment", ALLOCSET_DEFAULT_SIZES);
	MemoryContext prev = MemoryContextSwitchTo(cx);
	SiftColSegment *seg = sift_cseg_open(old, e, cx);
	SiftColBatch *in = sift_cbatch_create(cx, seg->schema);
	SiftColRow	row = {0};

	for (uint32 u = 0; u < seg->nsb; u++)
	{
		sift_cseg_decode_unit(seg, u, in);
		for (uint32 r = 0; r < in->nrows; r++)
		{
			SiftTblRowVer rv = {.key = in->keys[r],.wbuf = false,.xmin = in->hasver ? in->xmin[r] : InvalidTransactionId,.cmin = in->hasver ? in->cmin[r] : 0};

			if (!keep(&rv, arg))
				continue;
			sift_cbatch_row(in, r, &row);
			bulk_row(b, &row);
		}
		CHECK_FOR_INTERRUPTS();
	}
	sift_cseg_close(seg);
	MemoryContextSwitchTo(prev);
	MemoryContextDelete(cx);
}

/* The rows of a write buffer of the old table that keep accepts */
static void
bulk_wbuf(SiftTblBulk *b, Relation old, SiftTblSeg *seg, SiftTblKeep keep, void *arg)
{
	PGAlignedBlock copy;

	sift_tbl_seg_load(old, seg);
	for (uint32 p = 0; p < seg->npages; p++)
	{
		if (!seg_page(old, seg, p, copy.data))
			continue;
		for (uint32 off = 1; off <= TBL_PAGE(copy.data)->nrows; off++)
		{
			const SiftTblRec *rec = sift_tbl_page_rec(copy.data, seg->gen, off);
			ItemPointerData tid;
			SiftTblRowVer rv;

			if (rec == NULL)
				continue;
			ItemPointerSet(&tid, seg->vbase + p, off);
			sift_tbl_rowver(&rv, &tid, rec);
			if (!keep(&rv, arg))
				continue;
			sift_tbl_rowver_tuple(old, &rv, rec, seg->gen, &tid);
			sift_tbl_bulk_tuple(b, rv.tuple);
			heap_freetuple(rv.tuple);
		}
		CHECK_FOR_INTERRUPTS();
	}
}

bool
sift_tbl_bulk_copy(SiftTblBulk *b, Relation old, SiftTblRel *ost, SiftTblKeep keep, void *arg)
{
	SiftSegEntry *segs;
	int			n = 0;

	sift_tbl_refresh(old, ost);
	if (!bulk_same_layout(b, old, ost))
		return false;
	segs = palloc(Max(ost->dir.nsegs, 1) * sizeof(SiftSegEntry));
	for (int i = 0; i < ost->dir.nsegs; i++)
		if (ost->dir.segs[i].kind == SIFT_SEG_IMMUTABLE)
			segs[n++] = ost->dir.segs[i];
	qsort(segs, n, sizeof(SiftSegEntry), cmp_sb_lo);
	for (int i = 0; i < n; i++)
		bulk_segment(b, old, &segs[i], keep, arg);
	for (int i = 0; i < ost->nwbuf; i++)
		bulk_wbuf(b, old, &ost->wbuf[i], keep, arg);
	pfree(segs);
	return true;
}

void
sift_tbl_bulk_finish(SiftTblBulk *b)
{
	BulkPublish p;

	bulk_flush(b);
	if (b->sw.w)
		bulk_end_segment(b);
	p = (BulkPublish) {b->segs, b->nsegs, b->vbase + (b->nrows + SIFT_TBL_MAX_OFF - 1) / SIFT_TBL_MAX_OFF};
	if (b->nsegs > 0)
		sift_dir_update(b->sw.rel, &(SiftDirChange) {.fn = bulk_publish,.arg = &p,.extra = InvalidBuffer});
	sift_tbl_refresh(b->sw.rel, b->st);
	MemoryContextDelete(b->sw.cxt);
}
