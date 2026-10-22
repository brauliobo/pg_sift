/*
 * agg/stub_src.c - the column source of agg/colsrc.h over raw heap pages (the sift_colstub index; see agg/stub.c).
 *
 * A unit is the rows of one superblock. Its rows are read like an index that holds every entry until VACUUM would hold
 * them: no visibility check, every normal tuple that is not heap-only (a HOT chain is its root) and every redirect (its
 * values are the ones of the chain member it points to) is one row whose TID is the root line pointer; dead and unused
 * line pointers are not rows. The index datums of each row are kept (detoasted copies) in the unit's memory context and
 * every leaf is decoded from them when read, into aligned arrays with SIFT_AGG_PAD readable bytes behind. Everything a
 * unit owns lives in one context that is reset by the next stub_next, so memory is bounded by one superblock.
 *
 * A testing build only (SIFT_TESTING): the stand-in for the column index that the aggregate tests run on. It copies a heap page before it
 * reads the visibility map, so a VACUUM running under a scan could leave a pruned tuple in its rows (no guard, sift_vis_guard, is set); no
 * shipped build has the source, and no test runs a VACUUM under it.
 */
#include "postgres.h"

#ifdef SIFT_TESTING

#include <math.h>

#include "access/table.h"
#include "access/tableam.h"
#include "catalog/index.h"
#include "executor/executor.h"
#include "storage/bufmgr.h"
#include "utils/date.h"
#include "utils/datum.h"
#include "utils/fmgrprotos.h"
#include "utils/jsonb.h"
#include "utils/lsyscache.h"
#include "utils/numeric.h"

#include "agg/kern.h"
#include "agg/stub.h"
#include "core/cmp.h"
#include "core/visibility.h"
#include "agg/stub_src_int.h"

typedef struct StubSource StubSource;

struct StubSource
{
	MemoryContext cxt;
	MemoryContext ucxt;			/* the unit; reset by stub_next */
	Relation	heap;
	Relation	index;
	Snapshot	snapshot;
	StubLayout *lay;
	IndexInfo  *ii;
	EState	   *estate;
	TupleTableSlot *hslot;
	BufferAccessStrategy strategy;
	Page		page;			/* a copy of the heap page being read */
	Datum	   *vbuf;			/* the index datums of one tuple */
	bool	   *nbuf;
	BlockNumber nblocks;
	uint32		nsb;
	uint32		next_sb;
	uint32		end_sb;
	SiftSrcPrune *prune;
	int			nprune;
	int			cap;			/* rows the unit's arrays have room for */
	SiftKey    *rowkey;
	SiftVis		vis;
	bool		vis_open;
	StubUnit	unit;
	StubDecode	dc;
	ErrorSaveContext esc;
	SiftSrcCounters cnt;
};

static StubSource *
stub_open(Relation heap, Relation index, Snapshot snapshot, MemoryContext mcxt, bool lenient)
{
	MemoryContext cxt = AllocSetContextCreate(mcxt, "sift_colstub source", ALLOCSET_DEFAULT_SIZES);
	MemoryContext old = MemoryContextSwitchTo(cxt);
	StubSource *src = palloc0(sizeof(StubSource));
	int			natts;

	if (!index)
		elog(ERROR, "sift_colstub: a source needs a sift_colstub index");
	src->cxt = cxt;
	src->heap = heap;
	src->index = index;
	src->snapshot = snapshot;
	src->lay = sift_aggstub_layout(index);
	natts = src->lay->l.natts;
	src->ii = BuildIndexInfo(index);
	src->estate = CreateExecutorState();
	src->hslot = MakeSingleTupleTableSlot(RelationGetDescr(heap), &TTSOpsHeapTuple);
	GetPerTupleExprContext(src->estate)->ecxt_scantuple = src->hslot;
	src->strategy = GetAccessStrategy(BAS_BULKREAD);
	src->page = palloc(BLCKSZ);
	src->vbuf = palloc(sizeof(Datum) * natts);
	src->nbuf = palloc(sizeof(bool) * natts);
	src->nblocks = RelationGetNumberOfBlocks(heap);
	src->nsb = (src->nblocks + SIFT_SB_PAGES - 1) / SIFT_SB_PAGES;
	src->ucxt = AllocSetContextCreate(cxt, "sift_colstub unit", ALLOCSET_DEFAULT_SIZES);
	src->esc.type = T_ErrorSaveContext;
	src->dc.lay = src->lay;
	src->dc.cxt = src->ucxt;
	src->dc.idcxt = cxt;
	src->dc.tmp = AllocSetContextCreate(cxt, "sift_colstub row", ALLOCSET_DEFAULT_SIZES);
	src->dc.esc = lenient ? &src->esc : NULL;
	MemoryContextSwitchTo(old);
	return src;
}

static void *
stub_open_source(Relation heap, Relation index, Snapshot snapshot, MemoryContext mcxt)
{
	return stub_open(heap, index, snapshot, mcxt, false);
}

static void
stub_close(void *impl)
{
	StubSource *src = impl;

	if (src->vis_open)
		sift_vis_end(&src->vis);
	FreeExecutorState(src->estate);
	ExecDropSingleTupleTableSlot(src->hslot);
	FreeAccessStrategy(src->strategy);
	MemoryContextDelete(src->cxt);
}

static const SiftSrcLayout *
stub_source_layout(const void *impl)
{
	const StubSource *src = impl;
	return &src->lay->l;
}

static uint32
stub_nsb(void *impl)
{
	StubSource *src = impl;

	return src->nsb;
}

static void
stub_counters(const void *impl, SiftSrcCounters *c)
{
	const StubSource *src = impl;

	*c = src->cnt;
	c->bytes = src->dc.bytes;
	c->heap_pages = src->vis_open ? src->vis.heap_pages : 0;
}

static void
stub_begin(void *impl, uint32 sb_lo, uint32 sb_hi, const SiftSrcPrune *prune, int nprune)
{
	StubSource *src = impl;

	src->next_sb = sb_lo;
	src->end_sb = Min(sb_hi, src->nsb);
	src->nprune = nprune;
	src->prune = nprune ? MemoryContextAlloc(src->cxt, sizeof(SiftSrcPrune) * nprune) : NULL;
	if (nprune)
		memcpy(src->prune, prune, sizeof(SiftSrcPrune) * nprune);
}

static Datum
copy_att(Form_pg_attribute att, Datum d)
{
	if (att->attbyval)
		return d;
	if (att->attlen == -1)
		return PointerGetDatum(pg_detoast_datum_copy((struct varlena *) DatumGetPointer(d)));
	return datumCopy(d, false, att->attlen);
}

static void
add_row(StubSource *src, BlockNumber blk, OffsetNumber off, HeapTupleHeader htup, Size len)
{
	StubUnit   *u = &src->unit;
	int			natts = u->natts;
	HeapTupleData tup;
	MemoryContext old;

	tup.t_len = len;
	ItemPointerSet(&tup.t_self, blk, off);
	tup.t_tableOid = RelationGetRelid(src->heap);
	tup.t_data = htup;
	ExecStoreHeapTuple(&tup, src->hslot, false);
	FormIndexDatum(src->ii, src->hslot, src->estate, src->vbuf, src->nbuf);
	old = MemoryContextSwitchTo(src->ucxt);
	if (u->nrow == src->cap)
	{
		src->cap *= 2;
		u->vals = repalloc(u->vals, sizeof(Datum) * natts * src->cap);
		u->nulls = repalloc(u->nulls, sizeof(bool) * natts * src->cap);
		src->rowkey = repalloc(src->rowkey, sizeof(SiftKey) * src->cap);
	}
	for (int i = 0; i < natts; i++)
	{
		u->nulls[u->nrow * natts + i] = src->nbuf[i];
		u->vals[u->nrow * natts + i] = src->nbuf[i] ? (Datum) 0 : copy_att(TupleDescAttr(src->index->rd_att, i), src->vbuf[i]);
	}
	MemoryContextSwitchTo(old);
	ResetExprContext(GetPerTupleExprContext(src->estate));
	src->rowkey[u->nrow++] = sift_key_from_tid(&tup.t_self);
}

static void
read_page(StubSource *src, BlockNumber blk)
{
	Buffer		buf = ReadBufferExtended(src->heap, MAIN_FORKNUM, blk, RBM_NORMAL, src->strategy);

	LockBuffer(buf, BUFFER_LOCK_SHARE);
	memcpy(src->page, BufferGetPage(buf), BLCKSZ);
	UnlockReleaseBuffer(buf);
}

static void
scan_page(StubSource *src, BlockNumber blk)
{
	Page		page = src->page;
	OffsetNumber maxoff;

	read_page(src, blk);
	if (PageIsNew(page))
		return;
	maxoff = PageGetMaxOffsetNumber(page);
	for (OffsetNumber off = FirstOffsetNumber; off <= maxoff; off++)
	{
		ItemId		iid = PageGetItemId(page, off);
		ItemId		target = iid;

		if (ItemIdIsRedirected(iid))
			target = PageGetItemId(page, ItemIdGetRedirect(iid));
		if (!ItemIdIsNormal(target))
			continue;
		if (target == iid && HeapTupleHeaderIsHeapOnly((HeapTupleHeader) PageGetItem(page, iid)))
			continue;
		add_row(src, blk, off, (HeapTupleHeader) PageGetItem(page, target), ItemIdGetLength(target));
	}
}

/* The rows of superblock sb into src->unit; false if it has none */
static bool
load_sb(StubSource *src, uint32 sb)
{
	StubUnit   *u = &src->unit;
	BlockNumber end = Min((uint64) src->nblocks, ((uint64) sb + 1) * SIFT_SB_PAGES);
	SiftKey    *key;

	u->natts = src->lay->l.natts;
	src->cap = 1024;
	u->vals = MemoryContextAlloc(src->ucxt, sizeof(Datum) * u->natts * src->cap);
	u->nulls = MemoryContextAlloc(src->ucxt, sizeof(bool) * u->natts * src->cap);
	src->rowkey = MemoryContextAlloc(src->ucxt, sizeof(SiftKey) * src->cap);
	for (BlockNumber blk = sb * SIFT_SB_PAGES; blk < end; blk++)
		scan_page(src, blk);
	if (u->nrow == 0)
		return false;
	key = alloc_vec(src->ucxt, sizeof(SiftKey) * u->nrow);
	memcpy(key, src->rowkey, sizeof(SiftKey) * u->nrow);
	src->rowkey = key;
	return true;
}

static const StubVecSlot *
src_slot(StubSource *src, int leaf)
{
	return leaf_slot(&src->dc, &src->unit, leaf);
}

/* does the unit hold a row the prune entry may select: a value of its leaf in range, or for a junction every kid (some kid, any) may? */
static bool
prune_may(StubSource *src, const SiftSrcPrune *p)
{
	const SiftSrcLeaf *lf;
	const SiftSrcVec *v;
	bool		some = false;

	if (p->nkids > 0)
	{
		for (int i = 0; i < p->nkids; i++)
			if (prune_may(src, &p->kids[i]) == p->any)
				return p->any;
		return !p->any;
	}
	lf = &src->lay->l.leaf[p->leaf];
	if (lf->parent >= 0 || lf->kind != SCL_SCALAR || (lf->vtype != SCV_I32 && lf->vtype != SCV_I64))
		return true;
	v = &src_slot(src, p->leaf)->vec;
	for (int64 i = 0; i < v->n && !some; i++)
	{
		int64		x = lf->vtype == SCV_I32 ? ((const int32 *) v->data)[i] : ((const int64 *) v->data)[i];

		some = !(v->isnull && v->isnull[i]) && x >= p->lo && x <= p->hi && (p->nvals == 0 || bsearch(&x, p->vals, p->nvals, sizeof(int64), sift_cmp_i64) != NULL);
	}
	return some;
}

/* True if some prune entry may select no row of the unit */
static bool
unit_pruned(StubSource *src)
{
	for (int k = 0; k < src->nprune; k++)
		if (!prune_may(src, &src->prune[k]))
			return true;
	return false;
}

static bool
stub_next(void *impl, SiftSrcUnit *unit)
{
	StubSource *src = impl;

	while (src->next_sb < src->end_sb)
	{
		uint32		sb = src->next_sb++;

		MemoryContextReset(src->ucxt);
		memset(&src->unit, 0, sizeof(StubUnit));
		if (!load_sb(src, sb))
			continue;
		if (unit_pruned(src))
		{
			src->cnt.units_pruned++;
			continue;
		}
		unit->sb = sb;
		unit->nrow = src->unit.nrow;
		unit->priv = &src->unit;
		src->cnt.units++;
		src->cnt.rows += unit->nrow;
		return true;
	}
	return false;
}

static void
stub_read(void *impl, const SiftSrcUnit *unit, int leaf, SiftSrcVec *vec)
{
	StubSource *src = impl;
	Assert(unit->priv == &src->unit);
	*vec = src_slot(src, leaf)->vec;
}

const SiftKey *
sift_aggstub_keys(const SiftSrcSource *src)
{
	return ((const StubSource *) src->impl)->rowkey;
}

static void
stub_live(void *impl, const SiftSrcUnit *unit, uint64 *bits)
{
	int			nw = (unit->nrow + 63) / 64;

	memset(bits, 0xff, nw * sizeof(uint64));
	if (unit->nrow & 63)
		bits[nw - 1] = (UINT64CONST(1) << (unit->nrow & 63)) - 1;
}

/* The rows of one heap page, collected in order */
typedef struct PageRows
{
	BlockNumber blk;
	int			n;
	uint32		row[MaxHeapTuplesPerPage];
	uint64		offmask[SIFT_OFFMASK_WORDS];
} PageRows;

static void
page_flush(StubSource *src, PageRows *pr, uint64 *bits)
{
	if (pr->n == 0)
		return;
	sift_vis_filter_page(&src->vis, pr->blk, pr->offmask);
	src->cnt.rows_vetted += pr->n;
	for (int q = 0; q < pr->n; q++)
	{
		uint32		bit = (src->rowkey[pr->row[q]] & 0xFFFF) - 1;

		if (!((pr->offmask[bit >> 6] >> (bit & 63)) & 1))
			bits[pr->row[q] >> 6] &= ~(UINT64CONST(1) << (pr->row[q] & 63));
	}
	pr->n = 0;
	memset(pr->offmask, 0, sizeof(pr->offmask));
}

static void
stub_vet(void *impl, const SiftSrcUnit *unit, uint64 *bits)
{
	StubSource *src = impl;
	uint64		allvis[SIFT_PAGEMASK_WORDS];
	PageRows   *pr;

	if (!src->vis_open)
	{
		sift_vis_begin(&src->vis, src->heap, src->snapshot, NULL, 0);
		src->vis_open = true;
	}
	sift_vis_sb_mask(&src->vis, unit->sb, allvis);
	pr = palloc0(sizeof(PageRows));
	pr->blk = InvalidBlockNumber;
	for (int w = 0; w < (unit->nrow + 63) / 64; w++)
		for (uint64 m = bits[w]; m; m &= m - 1)
		{
			int			r = w * 64 + pg_rightmost_one_pos64(m);
			BlockNumber blk = sift_key_block(src->rowkey[r]);
			uint32		page = blk & (SIFT_SB_PAGES - 1),
						bit = (src->rowkey[r] & 0xFFFF) - 1;

			if ((allvis[page >> 6] >> (page & 63)) & 1)
				continue;
			if (blk != pr->blk)
			{
				page_flush(src, pr, bits);
				pr->blk = blk;
			}
			pr->offmask[bit >> 6] |= UINT64CONST(1) << (bit & 63);
			pr->row[pr->n++] = r;
		}
	page_flush(src, pr, bits);
	pfree(pr);
}

/* What the sample of two units says; the numbers that follow pg_class are not part of it, they are taken at every call */
typedef struct StubSample
{
	BlockNumber nblocks;		/* of the heap when sampled */
	double		rows_per_page;
	double		maxrows;		/* rows of the largest sampled unit */
	SiftSrcStats st;			/* ndistinct of the sample (not yet scaled), min, max, elems_per_row */
} StubSample;

/* Distinct non-null values of the vector, and the smallest and largest as doubles (finite numbers only) */
static double
vec_summary(SiftSrcVecType vt, const SiftSrcVec *v, double *mn, double *mx, bool *any)
{
	int64		n = 0;
	double	   *f;
	int64	   *i;
	double		distinct = 0;

	if (vt == SCV_CODE)
		return v->dict->n;
	f = palloc(sizeof(double) * (v->n + 1));
	i = (int64 *) f;
	for (int64 k = 0; k < v->n; k++)
	{
		if (v->isnull && v->isnull[k])
			continue;
		if (vt == SCV_F64)
			f[n++] = ((const double *) v->data)[k];
		else
			i[n++] = vt == SCV_I32 ? ((const int32 *) v->data)[k] : ((const int64 *) v->data)[k];
	}
	if (vt == SCV_F64)
		qsort(f, n, sizeof(double), sift_cmp_f64);
	else
		qsort(i, n, sizeof(int64), sift_cmp_i64);
	for (int64 k = 0; k < n; k++)
	{
		double		x = vt == SCV_F64 ? f[k] : (double) i[k];

		distinct += k == 0 || (vt == SCV_F64 ? f[k] != f[k - 1] : i[k] != i[k - 1]);
		if (isfinite(x))
		{
			*mn = *any ? Min(*mn, x) : x;
			*mx = *any ? Max(*mx, x) : x;
			*any = true;
		}
	}
	pfree(f);
	return distinct;
}

/* Folds one sampled unit into the statistics: the largest distinct count, the extremes, the elements of the groups */
static void
sample_unit(StubSource *src, const SiftSrcUnit *unit, SiftSrcStats *st, bool *seen, double *elems)
{
	const SiftSrcLayout *l = &src->lay->l;

	for (int leaf = 0; leaf < l->nleaves; leaf++)
	{
		const SiftSrcLeaf *lf = &l->leaf[leaf];
		SiftSrcVec	v;
		double		mn = 0,
					mx = 0;
		bool		any = false;

		stub_read(src, unit, leaf, &v);
		if (lf->kind != SCL_SCALAR)
		{
			for (int64 k = 0; k < v.n; k++)
				elems[leaf] += (v.isnull && v.isnull[k]) ? 0 : ((const int32 *) v.data)[k];
			continue;
		}
		st->ndistinct[leaf] = Max(st->ndistinct[leaf], vec_summary(lf->vtype, &v, &mn, &mx, &any));
		if (any)
		{
			st->min[leaf] = seen[leaf] ? Min(st->min[leaf], mn) : mn;
			st->max[leaf] = seen[leaf] ? Max(st->max[leaf], mx) : mx;
			seen[leaf] = true;
		}
	}
}

static void
sample_stats(Relation heap, Relation index, BlockNumber nblocks, StubSample *sm)
{
	MemoryContext cxt = AllocSetContextCreate(CurrentMemoryContext, "sift_colstub stats", ALLOCSET_DEFAULT_SIZES);
	StubSource *src = stub_open(heap, index, NULL, cxt, true);
	uint32		sbs[2] = {0, src->nsb / 2};
	double		rows = 0;
	double		pages = 0;
	bool		seen[SIFT_COLSRC_MAX_LEAVES] = {false};
	double		elems[SIFT_COLSRC_MAX_LEAVES] = {0};
	SiftSrcUnit unit;

	memset(sm, 0, sizeof(*sm));
	sm->nblocks = nblocks;
	for (int s = 0; s < (src->nsb > 1 ? 2 : 1); s++)
	{
		stub_begin(src, sbs[s], sbs[s] + 1, NULL, 0);
		pages += Min((uint64) nblocks - (uint64) sbs[s] * SIFT_SB_PAGES, SIFT_SB_PAGES);
		if (!stub_next(src, &unit))
			continue;
		rows += unit.nrow;
		sm->maxrows = Max(sm->maxrows, unit.nrow);
		sample_unit(src, &unit, &sm->st, seen, elems);
	}
	sm->rows_per_page = pages > 0 ? rows / pages : 0;
	for (int leaf = 0; leaf < src->lay->l.nleaves; leaf++)
		if (src->lay->l.leaf[leaf].kind != SCL_SCALAR)
			sm->st.elems_per_row[leaf] = rows > 0 ? elems[leaf] / rows : 0;
	stub_close(src);
	MemoryContextDelete(cxt);
}

static void
stub_stats(Relation rel, SiftSrcStats *st)
{
	Relation	heap = table_open(rel->rd_index->indrelid, AccessShareLock);
	BlockNumber nblocks = RelationGetNumberOfBlocks(heap);
	StubSample *sm = rel->rd_amcache;
	const SiftSrcLayout *l = &sift_aggstub_layout(rel)->l;

	if (!sm || nblocks * 10 > sm->nblocks * 12 || nblocks * 12 < sm->nblocks * 10)
	{
		if (!sm)
			sm = rel->rd_amcache = MemoryContextAlloc(rel->rd_indexcxt, sizeof(StubSample));
		sample_stats(heap, rel, nblocks, sm);
	}
	*st = sm->st;
	st->nunits = (nblocks + SIFT_SB_PAGES - 1) / SIFT_SB_PAGES;
	if (heap->rd_rel->relpages > 0)
		st->allvisible_frac = Min(1.0, (double) heap->rd_rel->relallvisible / heap->rd_rel->relpages);
	st->nrows = heap->rd_rel->reltuples > 0 ? heap->rd_rel->reltuples : sm->rows_per_page * nblocks;
	for (int leaf = 0; leaf < l->nleaves; leaf++)	/* nearly unique in the sample: grows with the table */
		if (l->leaf[leaf].kind == SCL_SCALAR && l->leaf[leaf].parent < 0 && st->ndistinct[leaf] > 0.9 * sm->maxrows)
			st->ndistinct[leaf] = Min(st->nrows, st->ndistinct[leaf] * st->nrows / sm->maxrows);
	table_close(heap, AccessShareLock);
}

static bool
stub_owns_table(Oid tablerelam)
{
	return false;
}

static SiftSrcLayout *
stub_layout(Relation rel)
{
	return &sift_aggstub_layout(rel)->l;
}

static const SiftSrcProvider stub_provider = {
	.owns_index = sift_aggstub_owns_index,
	.owns_table = stub_owns_table,
	.layout = stub_layout,
	.stats = stub_stats,
	.open = stub_open_source,
	.close = stub_close,
	.source_layout = stub_source_layout,
	.nsb = stub_nsb,
	.begin = stub_begin,
	.next = stub_next,
	.read = stub_read,
	.live = stub_live,
	.vet = stub_vet,
	.counters = stub_counters,
};

void
sift_aggstub_register(void)
{
	sift_colsrc_register(&stub_provider);
}

#endif							/* SIFT_TESTING */
