/*
 * sift_col_widescan.c - predicates and reads of the wide leaves: a wide integer of two INT lanes is compared as a
 * pair of keys, (lane 0, lane 1) in turn, which the column source builds out of ordinary tests of the lanes. Only the public API of the source
 * and the kernel table are used; sift_col_scan.c calls into this file from sift_col_filter and sift_col_begin.
 */
#include "postgres.h"

#include "sift_col_am.h"
#include "sift_col_seg.h"

bool
sift_colw_is_wide_int(const SiftColDef *d)
{
	return sift_col_def_is_wide(d) && d->store == SIFT_COLS_INT;
}

/* The predicate op leaf v: the plain test of one lane, v the key of the constant */
static SiftColPred
lane_pred(int leaf, SiftColOp op, const SiftColValue *v)
{
	return (SiftColPred) {leaf, op, 1, v};
}

static SiftColSel *
sel_copy(const SiftColUnit *unit, const SiftColSel *base)
{
	SiftColSel *s = palloc(sizeof(SiftColSel));

	sift_col_sel_alloc(unit->src, base->n, s);
	memcpy(s->bits, base->bits, base->nwords * sizeof(uint64));
	s->count = base->count;
	return s;
}

static void
lane_filter(const SiftColUnit *unit, int leaf, SiftColOp op, int64 key, SiftColSel *sel)
{
	SiftColValue v = {.v.i = key};
	SiftColPred p = lane_pred(leaf, op, &v);

	if (sel->count > 0)
		sift_col_filter_lane(unit, &p, sel);
}

/* The rows of base with (lane 0, lane 1) op (hi, lo), op one of EQ GT GE LT LE; the rows that overflowed stay (the source's rule), NULL rows go */
static SiftColSel *
pair_cmp(const SiftColUnit *unit, int col, SiftColOp op, int64 hi, int64 lo, const SiftColSel *base)
{
	SiftColSel *a,
			   *b = sel_copy(unit, base);
	SiftColOp	strict = op == SIFT_COLOP_GE ? SIFT_COLOP_GT : op == SIFT_COLOP_LE ? SIFT_COLOP_LT : op;

	lane_filter(unit, col, SIFT_COLOP_EQ, hi, b);
	lane_filter(unit, col + 1, op, lo, b);
	if (op == SIFT_COLOP_EQ)
		return b;
	a = sel_copy(unit, base);
	lane_filter(unit, col, strict, hi, a);		/* lane 0 beyond the constant's */
	sift_colk->bits_or(a->bits, b->bits, a->nwords);
	sift_col_sel_count(a);
	return a;
}

static void
sel_assign(SiftColSel *dst, const SiftColSel *src)
{
	memcpy(dst->bits, src->bits, dst->nwords * sizeof(uint64));
	sift_col_sel_count(dst);
}

bool
sift_colw_filter(const SiftColUnit *unit, const SiftColPred *p, SiftColSel *sel)
{
	int			col = p->col;
	SiftColSel *r;

	switch (p->op)
	{
		case SIFT_COLOP_ISNULL:
		case SIFT_COLOP_NOTNULL:
			return sift_col_filter_lane(unit, p, sel);
		case SIFT_COLOP_EQ:
		case SIFT_COLOP_GT:
		case SIFT_COLOP_GE:
		case SIFT_COLOP_LT:
		case SIFT_COLOP_LE:
			r = pair_cmp(unit, col, p->op, p->vals[0].v.i, p->vals[0].lo, sel);
			break;
		case SIFT_COLOP_NE:
			{
				SiftColSel *lt = pair_cmp(unit, col, SIFT_COLOP_LT, p->vals[0].v.i, p->vals[0].lo, sel);
				SiftColSel *gt = pair_cmp(unit, col, SIFT_COLOP_GT, p->vals[0].v.i, p->vals[0].lo, sel);

				sift_colk->bits_or(lt->bits, gt->bits, lt->nwords);
				r = lt;
				break;
			}
		case SIFT_COLOP_IN:
			{
				SiftColSel *acc = sel_copy(unit, sel);

				memset(acc->bits, 0, acc->nwords * sizeof(uint64));
				for (int i = 0; i < p->nvals; i++)
				{
					SiftColSel *one = pair_cmp(unit, col, SIFT_COLOP_EQ, p->vals[i].v.i, p->vals[i].lo, sel);

					sift_colk->bits_or(acc->bits, one->bits, acc->nwords);
				}
				r = acc;
				break;
			}
		case SIFT_COLOP_NOTIN:
			{
				SiftColSel *cur = sel_copy(unit, sel);

				for (int i = 0; i < p->nvals && cur->count > 0; i++)
				{
					SiftColPred ne = {col, SIFT_COLOP_NE, 1, &p->vals[i]};

					sift_colw_filter(unit, &ne, cur);
				}
				r = cur;
				break;
			}
		default:
			elog(ERROR, "sift_col: operator %d on a wide integer leaf", p->op);
			return false;
	}
	sel_assign(sel, r);
	return sel->count > 0;
}

/*
 * Zone pruning of a wide predicate. The values of a unit lie in the box [min0, max0] x [min1, max1] of its two lane zones, so none is above
 * (max0, max1) nor below (min0, min1) as pairs: x >= c needs (max0, max1) >= c, x > c needs it strictly above, x <= c and x < c the same for the
 * least corner, x = c both corners and c inside the box, x IN needs one constant like that. A unit whose zone flags an overflow value (a NaN)
 * describes nothing and stays; NE and NOT IN keep every unit.
 */
static bool
zone_keeps(const SiftCZone *z0, const SiftCZone *z1, SiftColOp op, const SiftColValue *v)
{
	int			hi_cmp,
				lo_cmp;

	if ((z0->flags | z1->flags) & SIFT_COLZ_OVERFLOW)
		return true;
	if (z0->nnull == z0->nvals)
		return false;			/* nothing but NULL: no comparison matches */
	hi_cmp = sift_col_wide_cmp(z0->max, z1->max, v->v.i, v->lo);		/* greatest corner against the constant */
	lo_cmp = sift_col_wide_cmp(z0->min, z1->min, v->v.i, v->lo);		/* least corner */
	switch (op)
	{
		case SIFT_COLOP_GE:
			return hi_cmp >= 0;
		case SIFT_COLOP_GT:
			return hi_cmp > 0;
		case SIFT_COLOP_LE:
			return lo_cmp <= 0;
		case SIFT_COLOP_LT:
			return lo_cmp < 0;
		case SIFT_COLOP_EQ:
			return hi_cmp >= 0 && lo_cmp <= 0 && z0->min <= v->v.i && v->v.i <= z0->max && z1->min <= v->lo && v->lo <= z1->max;
		default:
			return true;
	}
}

void
sift_colw_prune(SiftColSegment *seg, const SiftColPred *p, uint64 *keep)
{
	if (p->op == SIFT_COLOP_NE || p->op == SIFT_COLOP_NOTIN)
		return;
	if (p->op == SIFT_COLOP_ISNULL || p->op == SIFT_COLOP_NOTNULL)
	{
		SiftCTest	t;

		sift_ctest_make(&seg->schema->cols[p->col], NULL, p, &t);
		sift_cseg_prune(seg, p->col, &t, keep);
		return;
	}
	for (uint32 u = 0; u < seg->nsb; u++)
	{
		SiftCZone	z0,
					z1;
		bool		any = false;

		if (!sift_col_bit_test(keep, u))
			continue;
		sift_cseg_zone(seg, p->col, u, &z0);
		sift_cseg_zone(seg, p->col + 1, u, &z1);
		for (int i = 0; !any && i < (p->op == SIFT_COLOP_IN ? p->nvals : 1); i++)
			any = zone_keeps(&z0, &z1, p->op == SIFT_COLOP_IN ? SIFT_COLOP_EQ : p->op, &p->vals[i]);
		if (!any)
			sift_col_bit_clear(keep, u);
	}
}

void
sift_col_read_lanes(const SiftColUnit *unit, int col, const SiftColSel *sel, SiftColVec *vecs)
{
	const SiftColDef *d = sift_col_def(unit->src, col);

	for (int k = 0; k < Max(d->lanes, 1); k++)
		sift_col_read(unit, col + k, sel, &vecs[k]);
}
