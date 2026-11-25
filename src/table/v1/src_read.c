/*
 * table/v1/src_read.c - the column source of SiftAgg over a sift_table: the reads (vectors, dictionaries, overflow values), the ordered top-N rows,
 * the zones, the liveness and the pre-aggregates of a unit.
 */
#include "postgres.h"

#include "access/htup_details.h"
#include "catalog/pg_type.h"
#include "utils/rel.h"

#include "agg/kern.h"
#include "agg/pre.h"
#include "col/v1/src_sel.h"
#include "agg/agg.h"
#include "col/col.h"
#include "col/am.h"
#include "col/v1/ktab.h"
#include "col/v1/pagg.h"
#include "col/v1/seg.h"
#include "table/v1/table_v1.h"

#include "src_int.h"

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
	((TblSource *) impl)->rd.raw[leaf] = true;
}

void
tbl_tolerate_ovf(void *impl, int leaf)
{
	((TblSource *) impl)->rd.tol[leaf] = true;
}

/* only a path leaf has types, and the layout offers the paths only when every segment has them */
bool
tbl_filter_types(void *impl, const SiftSrcUnit *unit, int leaf, uint32 types, uint64 *bits)
{
	TblSource  *t = impl;
	TblPart    *p = unit->priv;
	const SiftSrcLeaf *l = &t->rd.lay->leaf[leaf];

	sift_srcsel_filter_types(&t->rd.sel, p->src, &p->unit, p->col[leaf], l->typid, l->astext, types, bits);
	return sift_srcsel_rows_left(bits, unit->nrow);
}

void
tbl_read_types(void *impl, const SiftSrcUnit *unit, int leaf, uint8 *tags)
{
	TblSource  *t = impl;
	TblPart    *p = unit->priv;
	const SiftSrcLeaf *l = &t->rd.lay->leaf[leaf];

	sift_srcsel_read_types(&t->rd.sel, &p->unit, p->col[leaf], l->typid, l->astext, unit->nrow, tags);
}

void
tbl_read_jnull(void *impl, const SiftSrcUnit *unit, int leaf, uint64 *bits)
{
	TblSource  *t = impl;
	TblPart    *p = unit->priv;
	bool		compact = t->rd.sel.compact,
				known;

	sift_col_jnull_bits(&p->unit, p->col[leaf], compact ? t->rd.sel.idx : NULL, t->rd.sel.sel.count, bits, &known);
	if (!known)
		sift_srcsel_types_unknown_error();
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
	for (int k = 0; k < t->rd.nparts; k++)
	{
		const TblPart *p = &t->rd.part[k];
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
	z->inexact |= hi > t->rd.wbuf_lo && lo <= t->rd.wbuf_hi;
	return units;
}

/* The rows of bits are few: the unit goes on as them alone, renumbered as in a unit of a match set */
bool
tbl_narrow(void *impl, SiftSrcUnit *unit, const uint64 *bits)
{
	TblSource  *t = impl;
	TblPart    *p = unit->priv;
	uint32		nrow;

	if (t->rd.sel.compact)
		return false;
	sift_srcsel_narrow(&t->rd.sel, p->src, &p->unit, bits, &nrow);
	unit->nrow = nrow;
	return true;
}

void
tbl_counters(const void *impl, SiftSrcCounters *c)
{
	const TblSource *t = impl;

	memset(c, 0, sizeof(*c));
	for (int k = 0; k < t->rd.nparts; k++)
	{
		const SiftColCounters *cc = sift_col_counters(t->rd.part[k].src);

		c->units += cc->units;
		c->units_pruned += cc->units_pruned;
		c->rows += cc->rows_scanned;
		c->bytes += cc->bytes_decoded;
		c->rows_vetted += cc->rows_vetted;
		c->heap_pages += cc->heap_pages;
	}
	c->units_search_pruned = t->rd.sel.units_skipped;
	c->units_pruned += t->rd.sel.units_skipped;
}

/* ---- pre-aggregates: the sidecar every stored unit carries (docs/COL-PREAGG.md), answered while its superblock is plain and the unit clean ---- */

bool
tbl_unit_exact(void *impl, const SiftSrcUnit *unit)
{
	SiftColSegment *seg;
	TblPart    *p = unit->priv;

	return sift_col_unit_whole(&p->unit, &seg) || sift_col_unit_preagg_ok(&p->unit);
}

int
tbl_unit_dead(void *impl, const SiftSrcUnit *unit, int mode)
{
	TblPart    *p = unit->priv;

	if (mode == SSD_NET)
		return sift_col_unit_net(&p->unit);
	return sift_col_unit_dead(&p->unit, mode == SSD_OFF ? -1 : sift_agg_dead_delta, mode == SSD_BOUNDS);
}

/* The key table of a segment answers for the key leaf and the measures: each source asks for them as its own columns, and one that lacks any reads its units */
bool
tbl_whole(void *impl, int leaf, const int *meas, int nmeas)
{
	TblSource  *t = impl;
	bool		asked = false;

	for (int k = 0; k < t->rd.nparts; k++)
	{
		TblPart    *p = &t->rd.part[k];
		int			cm[SIFT_SRC_MAX_MEAS],
					i = 0;

		for (; i < nmeas && p->col[meas[i]] >= 0; i++)
			cm[i] = p->col[meas[i]];
		/* a selection of heap addresses (a semi-join's) is the rows of units, not of a segment */
		if (i == nmeas && p->col[leaf] >= 0 && t->rd.sel.keep == NULL)
		{
			sift_col_super_whole(p->src, p->col[leaf], cm, nmeas);
			asked = true;
		}
	}
	return asked;
}

uint32
tbl_super_align(void *impl, uint32 sb)
{
	TblSource  *t = impl;
	uint32		at = sb;

	for (int k = 0; k < t->rd.nparts; k++)
		at = Max(at, sift_col_super_align(t->rd.part[k].src, sb));
	return at;
}

uint32
tbl_super_span(void *impl)
{
	TblSource  *t = impl;
	uint32		span = 0;

	for (int k = 0; k < t->rd.nparts; k++)
		span = Max(span, sift_col_super_span(t->rd.part[k].src));
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
tbl_unit_hist(void *impl, const SiftSrcUnit *unit, int dim, const SiftSrcRollupNeed *need, SiftSrcHist *h)
{
	TblPart    *p = unit->priv;
	bool		cal = dim >= SIFT_SRC_CAL;
	int			col = p->col[dim - (cal ? SIFT_SRC_CAL : 0)];

	StaticAssertStmt(SIFT_SRC_CAL == SIFT_PAGG_CAL, "the calendar dims of the source and of the cube are numbered alike");
	return col >= 0 && sift_aggpre_unit_hist(&((TblSource *) impl)->pre, &p->unit, col + (cal ? SIFT_PAGG_CAL : 0), &p->schema->cols[col], p->col, p->leaf_of_col, need, h);
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

	return p->col[leaf] >= 0 && !t->rd.lay->leaf[leaf].wide && sift_aggpre_run_info(&t->rd.sel, unit, &p->unit, p->col[leaf], nvals, nnull, id) &&
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
