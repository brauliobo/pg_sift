/*
 * table/src_read.c - the column source of SiftAgg over a sift_table: the reads (vectors, dictionaries, overflow values), the ordered top-N rows,
 * the zones, the liveness and the pre-aggregates of a unit.
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
#include "col/ktab.h"
#include "col/pagg.h"
#include "col/seg.h"
#include "agg/colsrc.h"
#include "table/table.h"

#include "core/visibility.h"
#include "src_int.h"

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

void
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

bool
tbl_overflow(void *impl, const SiftSrcUnit *unit, int leaf)
{
	TblPart    *p = unit->priv;
	SiftColZone cz;

	if (p->col[leaf] < 0)
		return false;
	sift_col_zone(&p->unit, p->col[leaf], &cz);
	return cz.has_ovf;
}

void
tbl_set_raw(void *impl, int leaf)
{
	((TblSource *) impl)->raw[leaf] = true;
}

void
tbl_tolerate_ovf(void *impl, int leaf)
{
	((TblSource *) impl)->tol[leaf] = true;
}

/* only a path leaf has types, and the layout offers the paths only when every segment has them */
bool
tbl_filter_types(void *impl, const SiftSrcUnit *unit, int leaf, uint32 types, uint64 *bits)
{
	TblSource  *t = impl;
	TblPart    *p = unit->priv;
	const SiftSrcLeaf *l = &t->lay->leaf[leaf];

	sift_aggsel_filter_types(&t->sel, p->src, &p->unit, p->col[leaf], l->typid, l->astext, types, bits);
	return sift_aggsel_rows_left(bits, unit->nrow);
}

void
tbl_read_types(void *impl, const SiftSrcUnit *unit, int leaf, uint8 *tags)
{
	TblSource  *t = impl;
	TblPart    *p = unit->priv;
	const SiftSrcLeaf *l = &t->lay->leaf[leaf];

	sift_aggsel_read_types(&t->sel, &p->unit, p->col[leaf], l->typid, l->astext, unit->nrow, tags);
}

void
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

void
tbl_tids(void *impl, const SiftSrcUnit *unit, const uint32 *rows, int n, ItemPointerData *tids)
{
	uint32	   *ords = sift_aggsel_ords(&((TblSource *) impl)->sel, rows, n);

	sift_col_row_tids(&((TblPart *) unit->priv)->unit, ords, n, tids);
	pfree(ords);
}

/* A segment written before the column has no bound for it: the unit is never skipped by it */
void
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
uint32
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

void
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
void
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
bool
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

void
tbl_live(void *impl, const SiftSrcUnit *unit, uint64 *bits)
{
	TblPart    *p = unit->priv;

	sift_aggsel_live(&((TblSource *) impl)->sel, &p->unit, bits);
}

void
tbl_vet(void *impl, const SiftSrcUnit *unit, uint64 *bits)
{
	TblPart    *p = unit->priv;

	sift_aggsel_vet(&((TblSource *) impl)->sel, p->src, &p->unit, bits);
}

bool
tbl_vet_needed(void *impl, const SiftSrcUnit *unit)
{
	TblPart    *p = unit->priv;

	return sift_aggsel_vet_needed(&p->unit);
}

void
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

void
tbl_preagg(Relation rel, SiftSrcPreagg *pa)
{
	SiftCPaggSpec spec;
	const SiftColSchema *schema = sift_tbl_layout_schema(rel, sift_tbl_rel(rel));

	/* the segments are written with the spec of their layout, whose columns are the leaves of the table's */
	sift_tbl_pagg_spec(rel, schema, &spec);
	sift_ktab_spec(schema, &spec);
	sift_aggpre_spec(&spec, pa);
}

bool
tbl_unit_exact(void *impl, const SiftSrcUnit *unit)
{
	SiftColSegment *seg;
	TblPart    *p = unit->priv;

	return sift_col_unit_whole(&p->unit, &seg) || sift_col_unit_preagg_ok(&p->unit);
}

/* The key table of a segment answers for the key leaf and the measures: each source asks for them as its own columns, and one that lacks any reads its units */
bool
tbl_whole(void *impl, int leaf, const int *meas, int nmeas)
{
	TblSource  *t = impl;
	bool		asked = false;

	for (int k = 0; k < t->nparts; k++)
	{
		TblPart    *p = &t->part[k];
		int			cm[SIFT_SRC_MAX_MEAS],
					i = 0;

		for (; i < nmeas && p->col[meas[i]] >= 0; i++)
			cm[i] = p->col[meas[i]];
		/* a selection of heap addresses (a semi-join's) is the rows of units, not of a segment */
		if (i == nmeas && p->col[leaf] >= 0 && t->sel.keep == NULL)
		{
			sift_col_super_whole(p->src, p->col[leaf], cm, nmeas);
			asked = true;
		}
	}
	return asked;
}

uint32
tbl_super_span(void *impl)
{
	TblSource  *t = impl;
	uint32		span = 0;

	for (int k = 0; k < t->nparts; k++)
		span = Max(span, sift_col_super_span(t->part[k].src));
	return span;
}

void
tbl_split(void *impl, const SiftSrcUnit *unit)
{
	TblPart    *p = unit->priv;

	sift_col_unit_split(p->src, &p->unit);
}

/* a segment of an older layout may lack the column: the unit is scanned, and the scan reads the catalog's default for it */
bool
tbl_unit_agg(void *impl, const SiftSrcUnit *unit, int leaf, SiftSrcUnitAgg *agg)
{
	TblPart    *p = unit->priv;

	return p->col[leaf] >= 0 && sift_aggpre_unit_agg(&p->unit, p->col[leaf], &p->schema->cols[p->col[leaf]], agg);
}

bool
tbl_unit_hist(void *impl, const SiftSrcUnit *unit, int dim, SiftSrcHist *h)
{
	TblPart    *p = unit->priv;
	bool		cal = dim >= SIFT_SRC_CAL;
	int			col = p->col[dim - (cal ? SIFT_SRC_CAL : 0)];

	StaticAssertStmt(SIFT_SRC_CAL == SIFT_PAGG_CAL, "the calendar dims of the source and of the cube are numbered alike");
	return col >= 0 && sift_aggpre_unit_hist(&((TblSource *) impl)->pre, &p->unit, col + (cal ? SIFT_PAGG_CAL : 0), &p->schema->cols[col], p->leaf_of_col, h);
}

uint32
tbl_unit_rollup_tuples(void *impl, const SiftSrcUnit *unit, int list)
{
	TblPart    *p = unit->priv;

	return list < 0 || p->col[list] >= 0 ? sift_col_unit_rollup_tuples(&p->unit, list < 0 ? list : p->col[list]) : 0;
}

bool
tbl_unit_rollup(void *impl, const SiftSrcUnit *unit, int list, const SiftSrcRollupNeed *need, SiftSrcRollup *h)
{
	TblPart    *p = unit->priv;

	return sift_aggpre_unit_rollup(&((TblSource *) impl)->pre, &p->unit, p->schema, list, need, p->col, p->leaf_of_col, h);
}

bool
tbl_unit_rollup_miss(void *impl, const SiftSrcUnit *unit)
{
	return sift_col_unit_rollup_miss(&((TblPart *) unit->priv)->unit);
}

bool
tbl_unit_presence(void *impl, const SiftSrcUnit *unit, int leaf, SiftSrcPresence *p)
{
	TblPart    *part = unit->priv;

	return part->col[leaf] >= 0 && sift_aggpre_unit_presence(&part->unit, part->col[leaf], p);
}

/*
 * A segment of an older layout may lack the column, and a wide leaf is ordered by the images of its values, not the stored keys. A table writes the run of a column
 * its rows follow (the run of any other takes more than a unit may give it): the rows are the run, so walking it reads no faster than the unit does, and only
 * the head of the run is of use
 */
bool
tbl_run_info(void *impl, const SiftSrcUnit *unit, int leaf, bool walk, uint32 *nvals, uint32 *nnull, uint64 *id)
{
	TblSource  *t = impl;
	TblPart    *p = unit->priv;

	return p->col[leaf] >= 0 && !t->lay->leaf[leaf].wide && sift_aggpre_run_info(&t->sel, unit, &p->unit, p->col[leaf], nvals, nnull, id) &&
		!(walk && sift_col_run_identity(&p->unit, p->col[leaf]));
}

int
tbl_run_heads(void *impl, const SiftSrcUnit *unit, int leaf, SiftSrcHead *lo, uint32 *nlo, SiftSrcHead *hi, uint32 *nhi)
{
	TblPart    *p = unit->priv;

	return sift_aggpre_run_heads(&p->unit, p->col[leaf], lo, nlo, hi, nhi);
}

void
tbl_run_read(void *impl, const SiftSrcUnit *unit, int leaf, uint32 pos, uint32 cnt, uint32 *rows)
{
	TblPart    *p = unit->priv;

	sift_col_run_read(&p->unit, p->col[leaf], pos, cnt, rows);
}
