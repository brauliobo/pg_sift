/*
 * sift_table_src.c - the column source of SiftAgg over a sift_table (sift_colsrc.h, docs/COL-PLAN.md, docs/COL-TAM.md section 12).
 *
 * The leaves are the columns of the table (the layout of the column groups, sift_table_seg.c). The groups are read by the engine, one
 * source per column layout the segments were written for (a segment keeps its own after ALTER TABLE; its leaves are matched by attribute
 * number), the rows of the write buffers are fed to the source of the table's layout as in-memory batches, one per superblock, and the
 * rows the snapshot sees are the table's: a superblock without tombstones or write buffer rows is entirely visible, the others are filtered by
 * sift_tbl_filter_page like every other reader of the table does.
 */
#include "postgres.h"

#include <math.h>

#include "access/heapam.h"
#include "access/htup_details.h"
#include "catalog/pg_statistic.h"
#include "catalog/pg_type.h"
#include "commands/defrem.h"
#include "nodes/makefuncs.h"
#include "nodes/bitmapset.h"
#include "pgstat.h"
#include "utils/rel.h"
#include "utils/snapmgr.h"
#include "utils/syscache.h"

#include "sift_aggkern.h"
#include "sift_aggpre.h"
#include "sift_aggsel.h"
#include "sift_col.h"
#include "sift_col_am.h"
#include "sift_col_pagg.h"
#include "sift_col_seg.h"
#include "sift_colsrc.h"
#include "sift_table.h"
#include "sift_visibility.h"

typedef struct TblLeafBuf
{
	size_t		cap;			/* elements the buffers hold */
	void	   *data;
	uint8	   *isnull;
	uint64	   *ovf;			/* numeric leaves: the NaNs of the table, which the columns cannot hold */
} TblLeafBuf;

/* The source of one column layout */
typedef struct TblPart
{
	SiftColSource *src;
	const SiftColSchema *schema;
	int		   *col;			/* [nleaves] the leaf of this schema that holds layout leaf i, -1 when it has none (a column added later) */
	int		   *leaf_of_col;	/* [schema->ncols] the layout leaf a leaf of this schema holds, -1 when the table dropped it */
	SiftColUnit unit;			/* the head of this source's units, valid when have */
	bool		have;
} TblPart;

typedef struct TblSource
{
	Relation	rel;
	SiftTblRel *st;
	Snapshot	snapshot;
	MemoryContext cxt;
	SiftDirSnapshot *dir;
	SiftTblTags *tags;			/* the tagged groups of dir */
	SiftTblSeg *wbuf;			/* private copies of the write buffers */
	int			nwbuf;
	uint32		wbuf_lo;		/* the superblocks that hold write buffer rows: [wbuf_lo, wbuf_hi] */
	uint32		wbuf_hi;
	Bitmapset  *batched;		/* the superblocks whose write buffer rows are in the sources already */
	SiftSrcLayout *lay;
	TblPart    *part;
	int			nparts;
	int			last;			/* the part of the unit returned last, -1 at the start */
	SiftSrcDict *dict;			/* per leaf: the dictionary of the last read */
	SiftSrcDict *adict;			/* per leaf: the dictionary of its default in the segments that lack the column */
	TblLeafBuf *buf;			/* per leaf */
	SiftAggSel	sel;			/* the match set the units are restricted to */
	SiftAggPre	pre;			/* the histogram of the last unit_hist */
} TblSource;

static bool
tbl_owns_index(Oid indexrelam)
{
	return false;
}

static bool
tbl_owns_table(Oid tablerelam)
{
	Oid			am = get_am_oid("sift_table", true);

	return OidIsValid(am) && tablerelam == am;
}

static SiftSrcVecType
leaf_vtype(const SiftColDef *def)
{
	switch (def->store)
	{
		case SIFT_COLS_INT:
			return (def->type == SIFT_COLT_INT8 || def->type == SIFT_COLT_TIMESTAMP || def->type == SIFT_COLT_TIMESTAMPTZ ||
					def->type == SIFT_COLT_NUMERIC) ? SCV_I64 : SCV_I32;
		case SIFT_COLS_F4:
		case SIFT_COLS_F8:
			return SCV_F64;
		case SIFT_COLS_STR:
			return SCV_CODE;
		default:
			return SCV_I64;
	}
}

/* The planner's view of the table: a scalar leaf per column; the exact datum images (SIFT_COLS_VAR) are not values the executor reads */
static SiftSrcLayout *
tbl_layout(Relation rel)
{
	const SiftColSchema *cur = sift_tbl_layout_schema(rel, sift_tbl_rel(rel));
	TupleDesc	desc = RelationGetDescr(rel);
	SiftSrcLayout *lay = palloc0(sizeof(SiftSrcLayout));

	lay->heapoid = RelationGetRelid(rel);
	lay->is_table = true;
	lay->natts = desc->natts;
	for (int a = 1; a <= desc->natts; a++)
	{
		Form_pg_attribute att = TupleDescAttr(desc, a - 1);

		lay->attexprs = lappend(lay->attexprs, att->attisdropped ? NULL :
								(Node *) makeVar(1, a, att->atttypid, att->atttypmod, att->attcollation, 0));
	}
	lay->nleaves = cur->ncols - 1;	/* the last leaf holds the exceptions */
	lay->leaf = palloc0(sizeof(SiftSrcLeaf) * lay->nleaves);
	for (int i = 0; i < lay->nleaves; i++)
	{
		const SiftColDef *def = &cur->cols[i];

		lay->leaf[i] = (SiftSrcLeaf) {.id = i,.attno = def->attno,.parent = -1,.kind = SCL_SCALAR,.vtype = leaf_vtype(def),
		.typid = sift_col_def_typid(desc, def),.scale = def->scale,.lossy = def->store == SIFT_COLS_VAR};
	}
	return lay;
}

/*
 * The share of the superblocks whose stored units may answer from their pre-aggregates, those with no write buffer row and no tombstone (tbl_sb_mask),
 * estimated without reading either: a write buffer holds the superblocks of its vblock range, and each dead row the statistics count (a delete or an update
 * leaves one tombstone) is in one superblock, so d of them touch nsb * (1 - exp(-d / nsb)) of the nsb superblocks.
 */
static double
exact_share(Relation rel, SiftTblRel *st, double nsb)
{
	PgStat_StatTabEntry *tab = pgstat_fetch_stat_tabentry(RelationGetRelid(rel));
	double		dead = tab ? Max((double) tab->dead_tuples, 0.0) : 0.0,
				wbuf = 0.0;

	for (int i = 0; i < st->nwbuf; i++)
		wbuf += ceil((double) st->wbuf[i].vcap / SIFT_SB_PAGES);
	return Max(0.0, 1.0 - (wbuf + nsb * (1.0 - exp(-dead / nsb))) / nsb);
}

static void
tbl_stats(Relation rel, SiftSrcStats *st)
{
	const SiftColSchema *cur = sift_tbl_layout_schema(rel, sift_tbl_rel(rel));

	memset(st, 0, sizeof(*st));
	st->nrows = Max(rel->rd_rel->reltuples, 0);
	st->nunits = SIFT_TBL_NEXT_VBLOCK(&sift_tbl_rel(rel)->dir.meta) / SIFT_SB_PAGES + 1;
	st->allvisible_frac = 1.0;
	st->exact_frac = exact_share(rel, sift_tbl_rel(rel), st->nunits);
	/* distinct values of a column: what ANALYZE of the table found (negative: a share of the rows), 0 until it has run */
	for (int i = 0; i < SIFT_COLSRC_MAX_LEAVES && i < cur->ncols - 1; i++)
	{
		HeapTuple	tp = SearchSysCache3(STATRELATTINH, ObjectIdGetDatum(RelationGetRelid(rel)), Int16GetDatum(cur->cols[i].attno), BoolGetDatum(false));

		if (!HeapTupleIsValid(tp))
			continue;
		st->ndistinct[i] = ((Form_pg_statistic) GETSTRUCT(tp))->stadistinct;
		if (st->ndistinct[i] < 0)
			st->ndistinct[i] *= -st->nrows;
		ReleaseSysCache(tp);
	}
}

/*
 * The table's own mask, not the heap's map: an SB is plain when it holds no write buffer row and no tombstone among those synced when the source
 * opened (tbl_open), before it read any row. A VACUUM of the table clears the liveness of rows behind tombstones, and a scan that holds
 * those tombstones sends their SBs to the filter whatever liveness it reads later, so no read of the mask follows a liveness it should have
 * preceded and the source needs no directory guard (sift_vis_guard).
 */
static void
tbl_sb_mask(void *arg, uint32 sb, uint64 *pagemask)
{
	TblSource  *t = arg;
	bool		plain = (sb < t->wbuf_lo || sb > t->wbuf_hi) &&
		!sift_tbl_tomb_any(t->st, (uint64) sb * (SIFT_SB_PAGES << 16), (uint64) (sb + 1) * (SIFT_SB_PAGES << 16) - 1) &&
		sift_tbl_sb_inserted(t->rel, t->snapshot, t->tags, sb);

	memset(pagemask, plain ? 0xFF : 0, SIFT_PAGEMASK_WORDS * sizeof(uint64));
}

static int
tbl_filter_page(void *arg, BlockNumber blk, uint64 *offmask)
{
	TblSource  *t = arg;

	return sift_tbl_filter_page_tags(t->rel, t->snapshot, t->tags, blk, offmask);
}

/* The leaf of schema that holds the column of leaf i of the layout; a store of another kind is a column the table changed the type of */
static int
part_col(const TblPart *p, const SiftColDef *want)
{
	for (int j = 0; j < p->schema->ncols; j++)
		if (p->schema->cols[j].attno == want->attno)
		{
			if (p->schema->cols[j].store != want->store)
				elog(ERROR, "sift_table: a segment holds column %d in another storage than the table's", want->attno);
			return j;
		}
	return -1;
}

static void
part_open(TblSource *t, TblPart *p, const SiftColSchema *schema, const SiftColSchema *cur, const SiftColVisibility *vis, uint32 sbs)
{
	p->schema = schema;
	p->src = sift_col_open_table(t->rel, schema, t->dir, vis, sbs, t->cxt);
	p->col = palloc(t->lay->nleaves * sizeof(int));
	p->leaf_of_col = palloc(schema->ncols * sizeof(int));
	memset(p->leaf_of_col, -1, schema->ncols * sizeof(int));
	for (int i = 0; i < t->lay->nleaves; i++)
	{
		p->col[i] = part_col(p, &cur->cols[i]);
		if (p->col[i] >= 0)
			p->leaf_of_col[p->col[i]] = i;
	}
}

static void *
tbl_open(Relation heap, Relation index, Snapshot snapshot, MemoryContext mcxt)
{
	MemoryContext old = MemoryContextSwitchTo(mcxt);
	TblSource  *t = palloc0(sizeof(TblSource));
	SiftTblRel *st = sift_tbl_rel(heap);
	const SiftColSchema *cur;
	SiftColVisibility vis = {tbl_sb_mask, tbl_filter_page, NULL, t};
	uint32		sbs;

	t->rel = heap;
	t->st = st;
	t->cxt = mcxt;
	t->snapshot = RegisterSnapshot(snapshot);
	t->last = -1;
	sift_aggsel_init(&t->sel, mcxt);
	sift_aggpre_init(&t->pre, mcxt);
	sift_tbl_refresh(heap, st);
	sift_tbl_tomb_sync(heap, st);
	t->dir = palloc(sizeof(SiftDirSnapshot));
	*t->dir = st->dir;
	t->tags = sift_tbl_tags_build(heap, st, t->dir, mcxt);
	t->nwbuf = st->nwbuf;
	t->wbuf = t->nwbuf ? palloc(t->nwbuf * sizeof(SiftTblSeg)) : NULL;
	t->wbuf_lo = PG_UINT32_MAX;
	for (int i = 0; i < t->nwbuf; i++)
	{
		t->wbuf[i] = st->wbuf[i];
		sift_tbl_seg_load(heap, &t->wbuf[i]);
		if (t->wbuf[i].npages == 0)
			continue;
		t->wbuf_lo = Min(t->wbuf_lo, t->wbuf[i].vbase / SIFT_SB_PAGES);
		t->wbuf_hi = Max(t->wbuf_hi, (t->wbuf[i].vbase + t->wbuf[i].npages - 1) / SIFT_SB_PAGES);
	}
	t->lay = tbl_layout(heap);
	t->dict = palloc0(sizeof(SiftSrcDict) * t->lay->nleaves);
	t->adict = palloc0(sizeof(SiftSrcDict) * t->lay->nleaves);
	t->buf = palloc0(sizeof(TblLeafBuf) * t->lay->nleaves);
	sbs = SIFT_TBL_NEXT_VBLOCK(&t->dir->meta) / SIFT_SB_PAGES + 1;
	cur = sift_tbl_layout_schema(heap, st);
	t->part = palloc0((t->dir->nsegs + 1) * sizeof(TblPart));
	part_open(t, &t->part[t->nparts++], cur, cur, &vis, sbs);
	for (int i = 0; i < t->dir->nsegs; i++)
	{
		const SiftColSchema *schema;
		int			k;

		if (t->dir->segs[i].kind != SIFT_SEG_IMMUTABLE)
			continue;
		schema = sift_tbl_seg_schema(heap, st, &t->dir->segs[i]);
		for (k = 0; k < t->nparts && t->part[k].schema->hash != schema->hash; k++)
			;
		if (k == t->nparts)
			part_open(t, &t->part[t->nparts++], schema, cur, &vis, sbs);
	}
	MemoryContextSwitchTo(old);
	return t;
}

static void
tbl_close(void *impl)
{
	TblSource  *t = impl;

	for (int k = 0; k < t->nparts; k++)
		sift_col_close(t->part[k].src);
	UnregisterSnapshot(t->snapshot);
}

static const SiftSrcLayout *
tbl_source_layout(const void *impl)
{
	return ((const TblSource *) impl)->lay;
}

static uint32
tbl_nsb(void *impl)
{
	return sift_col_heap_sbs(((TblSource *) impl)->part[0].src);
}

/* The write buffer rows of the superblocks [sb_lo, sb_hi) that no begin has fed to the source of the table's layout yet */
static void
add_wbuf(TblSource *t, uint32 sb_lo, uint32 sb_hi)
{
	MemoryContext old = MemoryContextSwitchTo(t->cxt);
	uint32		hi = Min(sb_hi, t->wbuf_hi + 1);

	for (uint32 sb = Max(sb_lo, t->wbuf_lo); sb < hi; sb++)
		if (!bms_is_member(sb, t->batched))
		{
			SiftColBatch *b = sift_tbl_wbuf_batch(t->rel, t->st, t->wbuf, t->nwbuf, sb, t->cxt);

			sift_col_add_batches(t->part[0].src, &b, 1);
			t->batched = bms_add_member(t->batched, sb);
		}
	MemoryContextSwitchTo(old);
}

static void
tbl_begin(void *impl, uint32 sb_lo, uint32 sb_hi, const SiftSrcPrune *prune, int nprune)
{
	TblSource  *t = impl;

	add_wbuf(t, sb_lo, sb_hi);
	for (int k = 0; k < t->nparts; k++)
	{
		sift_col_begin(t->part[k].src, NULL, 0, sb_lo, sb_hi);
		t->part[k].have = sift_col_next(t->part[k].src, &t->part[k].unit);
	}
	t->last = -1;
}

/* The units of the sources in ascending superblock: each source holds its head, and the one returned last advances on the next call */
static bool
tbl_next(void *impl, SiftSrcUnit *unit)
{
	TblSource  *t = impl;

	for (;;)
	{
		TblPart    *best = NULL;
		uint32		nrow;

		if (t->last >= 0)
			t->part[t->last].have = sift_col_next(t->part[t->last].src, &t->part[t->last].unit);
		for (int k = 0; k < t->nparts; k++)
			if (t->part[k].have && (best == NULL || t->part[k].unit.sb < best->unit.sb))
				best = &t->part[k];
		if (best == NULL)
			return false;
		t->last = best - t->part;
		nrow = best->unit.nrows;
		if (t->sel.keep && !sift_aggsel_unit(&t->sel, &best->unit, &nrow))
			continue;
		unit->sb = best->unit.sb;
		unit->nrow = nrow;
		unit->priv = best;
		return true;
	}
}

static void
tbl_select(void *impl, const SiftFilter *keep)
{
	((TblSource *) impl)->sel.keep = keep;
}

static const size_t vtype_size[] = {sizeof(int32), sizeof(int64), sizeof(double), sizeof(uint32)};

/* The leaf's buffers for n elements; the tail up to SIFT_AGG_PAD elements is zero */
static TblLeafBuf *
leaf_buf(TblSource *t, int leaf, SiftSrcVecType vt, size_t n)
{
	TblLeafBuf *b = &t->buf[leaf];
	size_t		need = n + SIFT_AGG_PAD;

	if (b->cap < need)
	{
		if (b->data)
		{
			pfree(b->data);
			pfree(b->isnull);
			if (b->ovf)
				pfree(b->ovf);
		}
		b->cap = Max(need, 4096);
		b->data = MemoryContextAlloc(t->cxt, b->cap * 8);
		b->isnull = MemoryContextAlloc(t->cxt, b->cap);
		b->ovf = t->lay->leaf[leaf].typid == NUMERICOID ? MemoryContextAlloc(t->cxt, (b->cap / 64 + 1) * sizeof(uint64)) : NULL;
	}
	memset((char *) b->data + n * vtype_size[vt], 0, SIFT_AGG_PAD * vtype_size[vt]);
	return b;
}

/* The one-entry dictionary of the text default of a column that older segments lack; its id is stable for the life of the source */
static const SiftSrcDict *
absent_dict(TblSource *t, int leaf, Datum d)
{
	SiftSrcDict *dict = &t->adict[leaf];

	if (dict->id == NULL)
	{
		text	   *tx = DatumGetTextPP(d);
		uint32	   *off = MemoryContextAlloc(t->cxt, 2 * sizeof(uint32));
		char	   *bytes = MemoryContextAlloc(t->cxt, VARSIZE_ANY_EXHDR(tx) + 1);

		off[0] = 0;
		off[1] = VARSIZE_ANY_EXHDR(tx);
		memcpy(bytes, VARDATA_ANY(tx), off[1]);
		*dict = (SiftSrcDict) {dict, 1, off, bytes};
	}
	return dict;
}

/* A column the segment was written before has the value the catalog gives rows that lack it (null, or the constant of ADD COLUMN .. DEFAULT) */
static void
read_absent(TblSource *t, int leaf, size_t n, SiftSrcVec *vec)
{
	const SiftSrcLeaf *l = &t->lay->leaf[leaf];
	bool		isnull;
	Datum		d = getmissingattr(RelationGetDescr(t->rel), l->attno, &isnull);
	TblLeafBuf *b = leaf_buf(t, leaf, l->vtype, n);

	memset(b->data, 0, n * vtype_size[l->vtype]);
	*vec = (SiftSrcVec) {.data = b->data,.n = n};
	if (isnull)
	{
		memset(b->isnull, 1, n);
		vec->isnull = b->isnull;
		return;
	}
	for (size_t i = 0; i < n; i++)
		switch (l->typid)
		{
			case INT2OID:
				((int32 *) b->data)[i] = DatumGetInt16(d);
				break;
			case INT4OID:
			case DATEOID:
				((int32 *) b->data)[i] = DatumGetInt32(d);
				break;
			case BOOLOID:
				((int32 *) b->data)[i] = DatumGetBool(d);
				break;
			case INT8OID:
			case TIMESTAMPOID:
			case TIMESTAMPTZOID:
				((int64 *) b->data)[i] = DatumGetInt64(d);
				break;
			case NUMERICOID:
				((int64 *) b->data)[i] = sift_col_numeric_key(l->scale, d, SIFT_COL_ROUND_NEAREST);
				break;
			case FLOAT4OID:
				((double *) b->data)[i] = DatumGetFloat4(d);
				break;
			case FLOAT8OID:
				((double *) b->data)[i] = DatumGetFloat8(d);
				break;
		}
	if (l->vtype == SCV_CODE)
		vec->dict = absent_dict(t, leaf, d);
}

/* The engine's vector of a leaf as the executor's, in the leaf's buffers */
static void
fill_vec(TblSource *t, int leaf, const SiftColVec *vp, SiftSrcVec *vec)
{
	const SiftColVec v = *vp;
	const SiftSrcLeaf *l = &t->lay->leaf[leaf];
	TblLeafBuf *b;
	size_t		n = v.n;

	b = leaf_buf(t, leaf, l->vtype, n);
	/* the only value a numeric(p <= 18, s) column cannot hold is NaN: the executor evaluates those rows as the numeric NaN they are */
	if (v.ovf && b->ovf == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("sift: a column of the aggregate scan holds values its column group cannot store"),
				 errhint("Set sift.enable_agg to off for this query.")));
	switch (l->vtype)
	{
		case SCV_I32:
			for (size_t i = 0; i < n; i++)
				((int32 *) b->data)[i] = (int32) v.i[i];
			break;
		case SCV_I64:
			memcpy(b->data, v.i, n * sizeof(int64));
			break;
		case SCV_F64:
			memcpy(b->data, v.f, n * sizeof(double));
			break;
		case SCV_CODE:
			memcpy(b->data, v.codes, n * sizeof(uint32));
			t->dict[leaf] = (SiftSrcDict) {(const void *) (uintptr_t) v.dict->id, v.dict->n, v.dict->off, v.dict->bytes};
			break;
	}
	*vec = (SiftSrcVec) {.data = b->data,.dict = l->vtype == SCV_CODE ? &t->dict[leaf] : NULL,.n = n};
	if (v.ovf)
	{
		memcpy(b->ovf, v.ovf, SIFT_COL_NWORDS(n) * sizeof(uint64));
		vec->ovf = b->ovf;
	}
	if (v.nulls)
	{
		for (size_t i = 0; i < n; i++)
			b->isnull[i] = sift_colsrc_bit(v.nulls, i);
		vec->isnull = b->isnull;
	}
}

static void
tbl_read(void *impl, const SiftSrcUnit *unit, int leaf, SiftSrcVec *vec)
{
	TblSource  *t = impl;
	TblPart    *p = unit->priv;
	SiftColVec	v;

	if (p->col[leaf] < 0)
	{
		read_absent(t, leaf, unit->nrow, vec);
		return;
	}
	sift_aggsel_read(&t->sel, &p->unit, p->col[leaf], &v);
	fill_vec(t, leaf, &v, vec);
}

/* ---- ordered top-N (docs/COL-SEARCH.md): the rows of a unit in the numbering of the executor are the idx-th rows of the unit as stored when a match set compacted it ---- */

static uint32 *
stored_ords(TblSource *t, const uint32 *rows, int n)
{
	uint32	   *ords = palloc((Max(n, 1) + SIFT_COL_IDXPAD) * sizeof(uint32));

	for (int i = 0; i < n; i++)
		ords[i] = t->sel.keep && t->sel.compact ? t->sel.idx[rows[i]] : rows[i];
	memset(ords + n, 0, SIFT_COL_IDXPAD * sizeof(uint32));
	return ords;
}

static void
tbl_tids(void *impl, const SiftSrcUnit *unit, const uint32 *rows, int n, ItemPointerData *tids)
{
	uint32	   *ords = stored_ords(impl, rows, n);

	sift_col_row_tids(&((TblPart *) unit->priv)->unit, ords, n, tids);
	pfree(ords);
}

/* A segment written before the column has no bound for it: the unit is never skipped by it */
static void
tbl_zone(void *impl, const SiftSrcUnit *unit, int leaf, SiftSrcZone *z)
{
	TblPart    *p = unit->priv;
	SiftColZone cz;

	if (p->col[leaf] < 0)
	{
		*z = (SiftSrcZone) {.nvals = unit->nrow,.inexact = true};
		return;
	}
	sift_col_zone(&p->unit, p->col[leaf], &cz);
	*z = (SiftSrcZone) {.min = cz.min,.max = cz.max,.nvals = cz.nvals,.nnull = cz.nnull,.inexact = cz.has_ovf};
}

/*
 * The zones of the stored units of the superblocks [lo, hi) together, from the segment headers of every column layout; a layout without the
 * column, and the write buffer rows (read whole), have no bound.
 */
static uint32
tbl_zone_range(void *impl, uint32 lo, uint32 hi, int leaf, SiftSrcZone *z)
{
	TblSource  *t = impl;
	uint32		units = 0;

	*z = (SiftSrcZone) {.min = PG_INT64_MAX,.max = PG_INT64_MIN};
	for (int k = 0; k < t->nparts; k++)
	{
		const TblPart *p = &t->part[k];
		SiftColZone cz;

		units += sift_col_zone_range(p->src, Max(p->col[leaf], 0), lo, hi, &cz);
		z->inexact |= cz.has_ovf || p->col[leaf] < 0;
		z->nvals += cz.nvals;
		z->nnull += cz.nnull;
		if (cz.nnull < cz.nvals)
		{
			z->min = Min(z->min, cz.min);
			z->max = Max(z->max, cz.max);
		}
	}
	z->inexact |= hi > t->wbuf_lo && lo <= t->wbuf_hi;
	return units;
}

static void
tbl_read_rows(void *impl, const SiftSrcUnit *unit, int leaf, const uint32 *rows, int n, SiftSrcVec *vec)
{
	TblSource  *t = impl;
	TblPart    *p = unit->priv;
	uint32	   *ords;
	SiftColVec	v;

	if (p->col[leaf] < 0)
	{
		read_absent(t, leaf, n, vec);
		return;
	}
	ords = stored_ords(t, rows, n);
	sift_col_read_idx(&p->unit, p->col[leaf], ords, n, &v);
	fill_vec(t, leaf, &v, vec);
	pfree(ords);
}

static void
tbl_live(void *impl, const SiftSrcUnit *unit, uint64 *bits)
{
	TblPart    *p = unit->priv;

	sift_aggsel_live(&((TblSource *) impl)->sel, &p->unit, bits);
}

static void
tbl_vet(void *impl, const SiftSrcUnit *unit, uint64 *bits)
{
	TblPart    *p = unit->priv;

	sift_aggsel_vet(&((TblSource *) impl)->sel, p->src, &p->unit, bits);
}

static bool
tbl_vet_needed(void *impl, const SiftSrcUnit *unit)
{
	TblPart    *p = unit->priv;

	return sift_aggsel_vet_needed(&p->unit);
}

static void
tbl_counters(const void *impl, SiftSrcCounters *c)
{
	const TblSource *t = impl;

	memset(c, 0, sizeof(*c));
	for (int k = 0; k < t->nparts; k++)
	{
		const SiftColCounters *cc = sift_col_counters(t->part[k].src);

		c->units += cc->units;
		c->units_pruned += cc->units_pruned;
		c->rows += cc->rows_scanned;
		c->bytes += cc->bytes_decoded;
		c->rows_vetted += cc->rows_vetted;
		c->heap_pages += cc->heap_pages;
	}
	c->units_pruned += t->sel.units_skipped;
}

/* ---- pre-aggregates: the sidecar every stored unit carries (docs/COL-PREAGG.md), answered while its superblock is plain and the unit clean ---- */

static void
tbl_preagg(Relation rel, SiftSrcPreagg *pa)
{
	SiftCPaggSpec spec;

	/* the segments are written with the default spec of their layout, whose columns are the leaves of the table's */
	sift_cpagg_spec_default(sift_tbl_layout_schema(rel, sift_tbl_rel(rel)), &spec);
	sift_aggpre_spec(&spec, pa);
}

static bool
tbl_unit_exact(void *impl, const SiftSrcUnit *unit)
{
	return sift_col_unit_preagg_ok(&((TblPart *) unit->priv)->unit);
}

/* a segment of an older layout may lack the column: the unit is scanned, and the scan reads the catalog's default for it */
static bool
tbl_unit_agg(void *impl, const SiftSrcUnit *unit, int leaf, SiftSrcUnitAgg *agg)
{
	TblPart    *p = unit->priv;

	return p->col[leaf] >= 0 && sift_aggpre_unit_agg(&p->unit, p->col[leaf], &p->schema->cols[p->col[leaf]], agg);
}

static bool
tbl_unit_hist(void *impl, const SiftSrcUnit *unit, int dim, SiftSrcHist *h)
{
	TblPart    *p = unit->priv;

	return p->col[dim] >= 0 &&
		sift_aggpre_unit_hist(&((TblSource *) impl)->pre, &p->unit, p->col[dim], &p->schema->cols[p->col[dim]], p->leaf_of_col, h);
}

static const SiftSrcProvider tbl_provider = {
	.owns_index = tbl_owns_index,
	.owns_table = tbl_owns_table,
	.layout = tbl_layout,
	.stats = tbl_stats,
	.open = tbl_open,
	.close = tbl_close,
	.source_layout = tbl_source_layout,
	.nsb = tbl_nsb,
	.begin = tbl_begin,
	.next = tbl_next,
	.read = tbl_read,
	.live = tbl_live,
	.vet = tbl_vet,
	.vet_needed = tbl_vet_needed,
	.counters = tbl_counters,
	.select = tbl_select,
	.tids = tbl_tids,
	.zone = tbl_zone,
	.zone_range = tbl_zone_range,
	.read_rows = tbl_read_rows,
	.preagg = tbl_preagg,
	.unit_exact = tbl_unit_exact,
	.unit_agg = tbl_unit_agg,
	.unit_hist = tbl_unit_hist,
};

void
sift_tbl_src_register(void)
{
	sift_colsrc_register(&tbl_provider);
}
