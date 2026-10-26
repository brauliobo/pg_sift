/*
 * table/src.c - the column source of SiftAgg over a sift_table (agg/colsrc.h, docs/COL-PLAN.md, docs/COL-TAM.md section 12).
 *
 * The leaves are the columns of the table (the layout of the column groups, table/seg.c). The groups are read by the engine, one
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
#include "common/hashfn.h"
#include "nodes/bitmapset.h"
#include "pgstat.h"
#include "optimizer/optimizer.h"
#include "optimizer/planner.h"
#include "rewrite/rewriteHandler.h"
#include "utils/memutils.h"
#include "utils/rel.h"
#include "utils/snapmgr.h"
#include "utils/syscache.h"
#include "utils/typcache.h"

#include "agg/kern.h"
#include "agg/pre.h"
#include "agg/sel.h"
#include "agg/wide.h"
#include "col/col.h"
#include "col/am.h"
#include "col/pagg.h"
#include "col/seg.h"
#include "agg/colsrc.h"
#include "table/table.h"
#include "table/src_int.h"
#include "core/visibility.h"

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

/* the layout keeps the tag of a type the table stores by size (money, time, enum, oid) as OTHER: an integer leaf is as wide as its column */
static SiftSrcVecType
leaf_vtype(const SiftColDef *def, TupleDesc desc)
{
	switch (def->store)
	{
		case SIFT_COLS_INT:
			return def->type == SIFT_COLT_NUMERIC || TupleDescAttr(desc, def->attno - 1)->attlen == 8 ? SCV_I64 : SCV_I32;
		case SIFT_COLS_F4:
		case SIFT_COLS_F8:
			return SCV_F64;
		case SIFT_COLS_STR:
			return SCV_CODE;
		default:
			return SCV_I64;
	}
}

/* numeric of any other shape than the narrow integer leaf, money and interval: a WHERE conjunct tests them on the exact values (SAP_WIDE) */
static bool
leaf_tested(const SiftColDef *def, Oid typid)
{
	return typid == CASHOID || typid == INTERVALOID || (typid == NUMERICOID && def->store == SIFT_COLS_VAR);
}

/* a leaf the executor reads through a dictionary of the datum images (agg/wide.h): the exact image of a column the table keeps by value as a size */
static bool
leaf_dict(const SiftColDef *def, Oid typid)
{
	return sift_aggwide_type(typid) && def->store != SIFT_COLS_INT;
}

/* The expression of a stored generated column, as the planner sees it in a query: a query that writes it reads the column (always the value the row holds) */
static Node *
generation_expr(Relation rel, Form_pg_attribute att)
{
	if (att->attgenerated != ATTRIBUTE_GENERATED_STORED)
		return NULL;
	return (Node *) expression_planner((Expr *) build_column_default(rel, att->attnum));
}

/* A leaf of a declared path of a jsonb column, not one of the table's own columns */
static bool
leaf_is_path(const SiftColDef *def)
{
	return def->attno > 0 && def->path[0];
}

/*
 * The planner's view of the table: a scalar leaf per column, the leaves of the declared paths after them (when every segment has them, else the
 * normal plan reads the jsonb column); the exact datum images (SIFT_COLS_VAR) are not values the executor reads
 */
static SiftSrcLayout *
tbl_layout(Relation rel)
{
	SiftTblRel *st = sift_tbl_rel(rel);
	const SiftColSchema *cur = sift_tbl_layout_schema(rel, st);
	TupleDesc	desc = RelationGetDescr(rel);
	SiftSrcLayout *lay = palloc0(sizeof(SiftSrcLayout));
	int			npath = 0;

	lay->heapoid = RelationGetRelid(rel);
	lay->is_table = true;
	lay->natts = desc->natts;
	for (int a = 1; a <= desc->natts; a++)
	{
		Form_pg_attribute att = TupleDescAttr(desc, a - 1);

		lay->attexprs = lappend(lay->attexprs, att->attisdropped ? NULL :
								(Node *) makeVar(1, a, att->atttypid, att->atttypmod, att->attcollation, 0));
		lay->attalias = lappend(lay->attalias, generation_expr(rel, att));
	}
	lay->nleaves = cur->ncols - 1;	/* the last leaf holds the exceptions */
	for (int i = 0; i < lay->nleaves; i++)
		npath += leaf_is_path(&cur->cols[i]);
	if (npath > 0 && !sift_tbl_paths_complete(rel, st))
		lay->nleaves -= npath;		/* the path leaves follow the columns */
	lay->leaf = palloc0(sizeof(SiftSrcLeaf) * lay->nleaves);
	for (int i = 0; i < lay->nleaves; i++)
	{
		const SiftColDef *def = &cur->cols[i];
		Oid			typid = sift_col_def_typid(desc, def);
		bool		wide = leaf_dict(def, typid);

		if (leaf_is_path(def))
		{
			sift_colsrc_def_leaf(cur->cols, i, desc, &lay->leaf[i]);
			continue;
		}
		lay->leaf[i] = (SiftSrcLeaf) {.id = i,.attno = def->attno,.parent = -1,.kind = SCL_SCALAR,.vtype = wide ? SCV_CODE : leaf_vtype(def, desc),
		.typid = typid,.wide = wide,.wtypid = leaf_tested(def, typid) ? typid : InvalidOid,.scale = def->scale,.lossy = !wide && def->store == SIFT_COLS_VAR};
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
	SiftTblRel *tr = sift_tbl_rel(rel);
	const SiftColSchema *cur = sift_tbl_layout_schema(rel, tr);
	SiftColLeafStats ls[SIFT_COL_MAX_COLS];
	bool		wovf[SIFT_COL_MAX_COLS],
				wjnull[SIFT_COL_MAX_COLS];

	memset(st, 0, sizeof(*st));
	st->nrows = Max(rel->rd_rel->reltuples, 0);
	st->nunits = SIFT_TBL_NEXT_VBLOCK(&tr->dir.meta) / SIFT_SB_PAGES + 1;
	st->allvisible_frac = 1.0;
	st->exact_frac = exact_share(rel, tr, st->nunits);
	sift_tbl_path_stats(rel, tr, ls, wovf, wjnull);
	sift_colsrc_leaf_stats(cur->cols, cur->ncols - 1, ls, wovf, wjnull, st);
	/* distinct values of a column: what ANALYZE of the table found (negative: a share of the rows), 0 until it has run */
	for (int i = 0; i < SIFT_COLSRC_MAX_LEAVES && i < cur->ncols - 1; i++)
	{
		HeapTuple	tp;

		if (leaf_is_path(&cur->cols[i]))
			continue;
		tp = SearchSysCache3(STATRELATTINH, ObjectIdGetDatum(RelationGetRelid(rel)), Int16GetDatum(cur->cols[i].attno), BoolGetDatum(false));
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
		sift_tbl_vers_sb(t->rel, t->snapshot, t->vers, sb) == SIFT_TBL_VER_ALL;

	memset(pagemask, plain ? 0xFF : 0, SIFT_PAGEMASK_WORDS * sizeof(uint64));
}

static int
tbl_filter_page(void *arg, BlockNumber blk, uint64 *offmask)
{
	TblSource  *t = arg;

	return sift_tbl_filter_page_vers(t->rel, t->snapshot, t->vers, blk, offmask);
}

/* The leaf of schema that holds leaf `want` of the layout: of the table's column (a store of another kind is a column the table changed the type of) or of a declared path (its def has to be the same) */
static int
part_col(const TblPart *p, const SiftColDef *want)
{
	for (int j = 0; j < p->schema->ncols; j++)
	{
		const SiftColDef *have = &p->schema->cols[j];

		if (have->attno != want->attno || strcmp(have->path ? have->path : "", want->path) != 0)
			continue;
		if (leaf_is_path(want))
		{
			if (have->kind == want->kind && have->store == want->store && have->type == want->type)
				return j;
			continue;
		}
		/* a uuid written before it was a dictionary is a VAR leaf of images: the segment is read through the datum, the next rewrite makes it a dictionary */
		SIFT_INVARIANT(have->store == want->store || (have->store == SIFT_COLS_VAR && want->store == SIFT_COLS_STR), "sift_table: a segment holds column %d in another storage than the table's", want->attno);
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
	sift_aggsel_init(&t->sel, mcxt, RelationGetRelid(heap));
	sift_aggpre_init(&t->pre, mcxt);
	sift_tbl_auto_defrag_tick(heap, st);
	sift_tbl_refresh(heap, st);
	sift_tbl_tomb_sync(heap, st);
	t->dir = palloc(sizeof(SiftDirSnapshot));
	*t->dir = st->dir;
	t->vers = sift_tbl_vers_build(heap, st, t->dir, mcxt);
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
	t->buf = palloc0(sizeof(SiftSrcLeafBuf) * t->lay->nleaves);
	t->tol = palloc0(sizeof(bool) * t->lay->nleaves);
	t->raw = palloc0(sizeof(bool) * t->lay->nleaves);
	t->wcxt = palloc0(sizeof(MemoryContext) * t->lay->nleaves);
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

/* The text tests join the zone pruning of every begin (a segment that lacks the column has no bound for it), and filter the rows of a unit on the encoded strings */
static void
tbl_prune_str(void *impl, const SiftSrcStrTest *t, int nt)
{
	TblSource  *tb = impl;

	for (int k = 0; k < tb->nparts; k++)
	{
		TblPart    *p = &tb->part[k];

		p->spred = MemoryContextAlloc(tb->cxt, Min(nt, COL_MAX_STR_PRUNE) * sizeof(SiftColPred));
		p->nspred = 0;
		for (int i = 0; i < nt && p->nspred < COL_MAX_STR_PRUNE; i++)
			if (p->col[t[i].leaf] >= 0 && !tb->lay->leaf[t[i].leaf].lossy)
				p->spred[p->nspred++] = sift_aggfilt_str_pred(&t[i], MemoryContextAlloc(tb->cxt, t[i].nvals * sizeof(SiftColValue)), p->col[t[i].leaf]);
	}
}

static void
filter_str_op(const SiftColUnit *cu, SiftColSel *sel, void *pred)
{
	sift_col_filter(cu, pred, 1, sel);
}

static bool
tbl_filter_str(void *impl, const SiftSrcUnit *unit, const SiftSrcStrTest *t, uint64 *rows)
{
	TblSource  *tb = impl;
	TblPart    *p = unit->priv;
	SiftColValue *vals;
	SiftColPred pred;

	if (p->col[t->leaf] < 0 || tb->lay->leaf[t->leaf].lossy)
		return false;
	vals = palloc(Max(t->nvals, 1) * sizeof(SiftColValue));
	pred = sift_aggfilt_str_pred(t, vals, p->col[t->leaf]);
	sift_aggsel_stored(&tb->sel, p->src, &p->unit, rows, filter_str_op, &pred);
	pfree(vals);
	return true;
}

static void
tbl_begin(void *impl, uint32 sb_lo, uint32 sb_hi, const SiftSrcPrune *prune, int nprune)
{
	TblSource  *t = impl;

	add_wbuf(t, sb_lo, sb_hi);
	for (int k = 0; k < t->nparts; k++)
	{
		TblPart    *p = &t->part[k];
		SiftColPred preds[SIFT_AGGZONE_MAX + COL_MAX_STR_PRUNE + SIFT_COLSRC_MAX_FILTERS];
		int			n = sift_aggzone_preds(&p->zones, t->cxt, p->col, prune, nprune, preds);

		for (int i = 0; i < p->nspred; i++)
			preds[n++] = p->spred[i];
		for (int i = 0; i < p->flt.n; i++)
			preds[n++] = p->flt.pred[i];
		sift_col_begin_zones(p->src, preds, n, p->zones.zone, p->zones.nzone, sb_lo, sb_hi);
		p->have = sift_col_next(p->src, &p->unit);
	}
	t->last = -1;
}

/* The units of the sources in ascending superblock: each source holds its head, and the one returned last advances on the next call */
static bool
tbl_next(void *impl, SiftSrcUnit *unit)
{
	TblSource  *t = impl;

	t->sel.compact = false;
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

/* A filter is the source's when every segment layout holds its column as an integer leaf: a segment without the column would need the catalog's default tested */
static uint64
tbl_set_filters(void *impl, const SiftSrcFilter *f, int nf)
{
	TblSource  *t = impl;
	uint64		ok = 0;

	for (int k = 0; k < t->nparts; k++)
		t->part[k].flt = (SiftAggFilters) {0};
	for (int i = 0; i < nf; i++)
	{
		bool		all = true;

		for (int k = 0; k < t->nparts; k++)
			all &= t->part[k].col[f[i].leaf] >= 0 && sift_aggfilt_int(&t->part[k].schema->cols[t->part[k].col[f[i].leaf]]);
		if (!all)
			continue;
		for (int k = 0; k < t->nparts; k++)
			sift_aggfilt_add(&t->part[k].flt, t->cxt, t->part[k].col[f[i].leaf], &f[i]);
		ok |= UINT64CONST(1) << i;
	}
	return ok;
}

static bool
tbl_filter(void *impl, const SiftSrcUnit *unit, uint64 *bits)
{
	TblSource  *t = impl;
	TblPart    *p = unit->priv;
	SiftAggFiltRun run = {&p->flt, t->lay, t->tol};

	sift_aggsel_stored(&t->sel, p->src, &p->unit, bits, sift_aggfilt_op, &run);
	return sift_aggsel_rows_left(bits, unit->nrow);
}

static void
tbl_select(void *impl, const SiftFilter *keep)
{
	((TblSource *) impl)->sel.keep = keep;
}

/* The dictionary of the text default of a column that older segments lack (one entry; none when the default is NULL); its id is stable for the life of the source */
static const SiftSrcDict *
absent_dict(TblSource *t, int leaf, Datum d, bool isnull)
{
	SiftSrcDict *dict = &t->adict[leaf];

	if (dict->id == NULL)
	{
		text	   *tx = isnull ? NULL : DatumGetTextPP(d);
		uint32	   *off = MemoryContextAllocZero(t->cxt, 2 * sizeof(uint32));
		char	   *bytes = MemoryContextAlloc(t->cxt, (tx ? VARSIZE_ANY_EXHDR(tx) : 0) + 1);

		if (tx)
		{
			off[1] = VARSIZE_ANY_EXHDR(tx);
			memcpy(bytes, VARDATA_ANY(tx), off[1]);
		}
		*dict = (SiftSrcDict) {dict, tx != NULL, off, bytes};
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
	SiftSrcLeafBuf *b;

	if (l->wide)
	{
		wide_read(t, leaf, NULL, NULL, n, d, isnull, vec);
		return;
	}
	b = &t->buf[leaf];
	sift_aggsel_leaf_buf(t->cxt, b, n);
	memset(b->data, 0, (n + SIFT_AGG_PAD) * sift_src_vtype_size[l->vtype]);
	*vec = (SiftSrcVec) {.data = b->data,.n = n};
	if (isnull)
	{
		memset(b->isnull, 1, n);
		vec->isnull = b->isnull;
		if (l->vtype == SCV_CODE)
			vec->dict = absent_dict(t, leaf, d, true);
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
			case TIMEOID:
			case CASHOID:
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
			default:
				if (l->vtype == SCV_I32)	/* an enum */
					((int32 *) b->data)[i] = (int32) DatumGetObjectId(d);
		}
	if (l->vtype == SCV_CODE)
		vec->dict = absent_dict(t, leaf, d, false);
}

/* A wide leaf is read through a dictionary of its datum images, unless the executor wants the codes of the stored dictionary (set_raw) and the segment has one (an older layout holds the images inline) */
static void
read_vec(TblSource *t, int leaf, const TblPart *p, const SiftColVec *v, SiftSrcVec *vec)
{
	if (t->lay->leaf[leaf].wide && !(t->raw[leaf] && v->dict != NULL))
		wide_read(t, leaf, p, v, v->n, (Datum) 0, false, vec);
	else
	{
		SiftSrcLeafBuf *b = &t->buf[leaf];

		sift_aggsel_leaf_buf(t->cxt, b, v->n);
		sift_aggsel_fill(&t->sel, &t->lay->leaf[leaf], t->tol[leaf], v, &(SiftSrcBuf) {b->data, b->isnull, b->ovf}, &t->dict[leaf], vec);
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
	if (!t->lay->leaf[leaf].wide && !t->sel.compact)
	{
		/* decoded into the leaf's buffer in its own vector type, with no vector of keys to convert and copy */
		SiftSrcLeafBuf *b = &t->buf[leaf];

		sift_aggsel_leaf_buf(t->cxt, b, sift_col_lane_n(&p->unit, p->col[leaf]));

		sift_aggsel_read_lane(&t->sel, &p->unit, p->col[leaf], &t->lay->leaf[leaf], t->tol[leaf], &(SiftSrcBuf) {b->data, b->isnull, b->ovf}, &t->dict[leaf], vec);
		return;
	}
	sift_aggsel_read(&t->sel, &p->unit, p->col[leaf], &v);
	read_vec(t, leaf, p, &v, vec);
}

/* ---- the values of a path of another JSON type than its leaf's: the engine sets them aside as overflow, with the tag of their type ---- */

static bool
tbl_overflow(void *impl, const SiftSrcUnit *unit, int leaf)
{
	TblPart    *p = unit->priv;
	SiftColZone cz;

	if (p->col[leaf] < 0)
		return false;
	sift_col_zone(&p->unit, p->col[leaf], &cz);
	return cz.has_ovf;
}

static void
tbl_set_raw(void *impl, int leaf)
{
	((TblSource *) impl)->raw[leaf] = true;
}

static void
tbl_tolerate_ovf(void *impl, int leaf)
{
	((TblSource *) impl)->tol[leaf] = true;
}

/* only a path leaf has types, and the layout offers the paths only when every segment has them */
static bool
tbl_filter_types(void *impl, const SiftSrcUnit *unit, int leaf, uint32 types, uint64 *bits)
{
	TblSource  *t = impl;
	TblPart    *p = unit->priv;
	const SiftSrcLeaf *l = &t->lay->leaf[leaf];

	sift_aggsel_filter_types(&t->sel, p->src, &p->unit, p->col[leaf], l->typid, l->astext, types, bits);
	return sift_aggsel_rows_left(bits, unit->nrow);
}

static void
tbl_read_types(void *impl, const SiftSrcUnit *unit, int leaf, uint8 *tags)
{
	TblSource  *t = impl;
	TblPart    *p = unit->priv;
	const SiftSrcLeaf *l = &t->lay->leaf[leaf];

	sift_aggsel_read_types(&t->sel, &p->unit, p->col[leaf], l->typid, l->astext, unit->nrow, tags);
}

static void
tbl_read_jnull(void *impl, const SiftSrcUnit *unit, int leaf, uint64 *bits)
{
	TblSource  *t = impl;
	TblPart    *p = unit->priv;
	bool		compact = t->sel.compact,
				known;

	sift_col_jnull_bits(&p->unit, p->col[leaf], compact ? t->sel.idx : NULL, t->sel.sel.count, bits, &known);
	if (!known)
		sift_aggsel_types_unknown_error();
}

/* ---- ordered top-N (docs/COL-SEARCH.md): the rows of a unit in the numbering of the executor are the idx-th rows of the unit as stored when a match set compacted it ---- */

static void
tbl_tids(void *impl, const SiftSrcUnit *unit, const uint32 *rows, int n, ItemPointerData *tids)
{
	uint32	   *ords = sift_aggsel_ords(&((TblSource *) impl)->sel, rows, n);

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
	ords = sift_aggsel_ords(&t->sel, rows, n);
	sift_col_read_idx(&p->unit, p->col[leaf], ords, n, &v);
	read_vec(t, leaf, p, &v, vec);
	pfree(ords);
}

/* Only the rows of bits, at their rows in the leaf's buffer; the unit as stored, a column the segment lacks and a wide leaf are read whole */
static void
tbl_read_sel(void *impl, const SiftSrcUnit *unit, int leaf, const uint64 *bits, SiftSrcVec *vec)
{
	TblSource  *t = impl;
	TblPart    *p = unit->priv;
	SiftColSel	sel;
	SiftColVec	v;
	uint32	   *idx;
	SiftSrcLeafBuf *b = &t->buf[leaf];

	if (t->sel.compact || p->col[leaf] < 0 || t->lay->leaf[leaf].wide)
	{
		tbl_read(impl, unit, leaf, vec);
		return;
	}
	sift_aggsel_bits(p->src, unit->nrow, bits, &sel);
	idx = palloc((sel.count + SIFT_COL_IDXPAD) * sizeof(uint32));
	sift_col_sel_idx(&sel, idx);
	sift_col_read(&p->unit, p->col[leaf], &sel, &v);
	sift_aggsel_leaf_buf(t->cxt, b, unit->nrow);
	sift_aggsel_scatter(&t->lay->leaf[leaf], t->tol[leaf], &v, idx, unit->nrow, &(SiftSrcBuf) {b->data, b->isnull, b->ovf}, &t->dict[leaf], vec);
	pfree(idx);
}

/* The rows of bits are few: the unit goes on as them alone, renumbered as in a unit of a match set */
static bool
tbl_narrow(void *impl, SiftSrcUnit *unit, const uint64 *bits)
{
	TblSource  *t = impl;
	TblPart    *p = unit->priv;
	uint32		nrow;

	if (t->sel.compact)
		return false;
	sift_aggsel_narrow(&t->sel, p->src, &p->unit, bits, &nrow);
	unit->nrow = nrow;
	return true;
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
	c->units_search_pruned = t->sel.units_skipped;
	c->units_pruned += t->sel.units_skipped;
}

/* ---- pre-aggregates: the sidecar every stored unit carries (docs/COL-PREAGG.md), answered while its superblock is plain and the unit clean ---- */

static void
tbl_preagg(Relation rel, SiftSrcPreagg *pa)
{
	SiftCPaggSpec spec;
	const SiftColSchema *schema = sift_tbl_layout_schema(rel, sift_tbl_rel(rel));

	/* the segments are written with the default spec of their layout (its calendar dims when the minor allows them: seg_write.c), whose columns are the leaves of the table's */
	sift_cpagg_spec_default(schema, &spec);
	if (sift_col_format_minor >= SIFT_COL_FORMAT_CALENDAR && !schema->kinds)
		sift_cpagg_spec_calendar(schema, &spec);
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
	bool		cal = dim >= SIFT_SRC_CAL;
	int			col = p->col[dim - (cal ? SIFT_SRC_CAL : 0)];

	StaticAssertStmt(SIFT_SRC_CAL == SIFT_PAGG_CAL, "the calendar dims of the source and of the cube are numbered alike");
	return col >= 0 && sift_aggpre_unit_hist(&((TblSource *) impl)->pre, &p->unit, col + (cal ? SIFT_PAGG_CAL : 0), &p->schema->cols[col], p->leaf_of_col, h);
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
	.read_sel = tbl_read_sel,
	.narrow = tbl_narrow,
	.prune_str = tbl_prune_str,
	.filter_str = tbl_filter_str,
	.set_filters = tbl_set_filters,
	.filter = tbl_filter,
	.set_wide = tbl_set_wide,
	.filter_wide = tbl_filter_wide,
	.overflow = tbl_overflow,
	.tolerate_ovf = tbl_tolerate_ovf,
	.set_raw = tbl_set_raw,
	.filter_types = tbl_filter_types,
	.read_types = tbl_read_types,
	.read_jnull = tbl_read_jnull,
};

void
sift_tbl_src_register(void)
{
	sift_colsrc_register(&tbl_provider);
}
