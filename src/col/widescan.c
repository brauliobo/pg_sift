/*
 * col/widescan.c - predicates and reads of the wide leaves: a wide integer of two INT lanes is compared as a
 * pair of keys, (lane 0, lane 1) in turn, which the column source builds out of ordinary tests of the lanes. Only the public API of the source
 * and the kernel table are used; col/scan.c and col/scan_filter.c call into this file from sift_col_begin and sift_col_filter.
 */
#include "postgres.h"

#include "utils/builtins.h"
#include "utils/numeric.h"
#include "utils/timestamp.h"

#include "col/am.h"
#include "col/seg.h"

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

SiftColSel *
sift_colw_sel_copy(const SiftColUnit *unit, const SiftColSel *base)
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
			   *b = sift_colw_sel_copy(unit, base);
	SiftColOp	strict = op == SIFT_COLOP_GE ? SIFT_COLOP_GT : op == SIFT_COLOP_LE ? SIFT_COLOP_LT : op;

	lane_filter(unit, col, SIFT_COLOP_EQ, hi, b);
	lane_filter(unit, col + 1, op, lo, b);
	if (op == SIFT_COLOP_EQ)
		return b;
	a = sift_colw_sel_copy(unit, base);
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
				SiftColSel *acc = sift_colw_sel_copy(unit, sel);

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
				SiftColSel *cur = sift_colw_sel_copy(unit, sel);

				for (int i = 0; i < p->nvals && cur->count > 0; i++)
				{
					SiftColPred ne = {col, SIFT_COLOP_NE, 1, &p->vals[i]};

					sift_colw_filter(unit, &ne, cur);
				}
				r = cur;
				break;
			}
		default:
			SIFT_INVARIANT(false, "sift_col: operator %d on a wide integer leaf", p->op);
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
		sift_cseg_prune(seg, p->col, &t, keep, 0, seg->nsb, NULL, true);
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

static bool
is_numeric_wide(const SiftColDef *d)
{
	return d->type == SIFT_COLT_NUMERIC && d->store == SIFT_COLS_INT && d->lanes == 2;
}

typedef enum NumClass
{
	NC_FINITE,
	NC_NAN,
	NC_PINF,
	NC_NINF
} NumClass;

static NumClass
num_class(Datum d)
{
	Numeric		n = DatumGetNumeric(d);

	if (numeric_is_nan(n))
		return NC_NAN;
	if (!numeric_is_inf(n))
		return NC_FINITE;
	return DatumGetCString(DirectFunctionCall1(numeric_out, d))[0] == '-' ? NC_NINF : NC_PINF;
}

/* the verdict of a comparison with NaN, +Infinity, -Infinity: PostgreSQL orders NaN above everything, itself equal */
static const SiftColWVerdict special_verdict[3][6] = {
	/* =                  <>                            <                             <=                            >                  >= */
	{SIFT_COLW_NAN, SIFT_COLW_NONNULL_NONAN, SIFT_COLW_NONNULL_NONAN, SIFT_COLW_NONNULL, SIFT_COLW_NONE, SIFT_COLW_NAN},
	{SIFT_COLW_NONE, SIFT_COLW_NONNULL, SIFT_COLW_NONNULL_NONAN, SIFT_COLW_NONNULL_NONAN, SIFT_COLW_NAN, SIFT_COLW_NAN},
	{SIFT_COLW_NONE, SIFT_COLW_NONNULL, SIFT_COLW_NONE, SIFT_COLW_NONE, SIFT_COLW_NONNULL, SIFT_COLW_NONNULL}
};

/* the two lane keys of a finite operand rounded as round asks; the status of sift_col_numeric_wide_key */
static int
wide_key(const SiftColDef *def, Datum d, SiftColRound round, SiftColValue *v)
{
	return sift_col_numeric_wide_key(def->scale, d, round, &v->v.i, &v->lo);
}

/* the key of an operand that some value of the leaf may equal; false when none does (a NaN or infinity of a lane pair, a number between two scaled integers or beyond them) */
static bool
exact_key(const SiftColDef *def, Datum d, SiftColValue *v)
{
	SiftColValue up = {{0}};
	int32		dscale;
	int64		md;

	memset(v, 0, sizeof(*v));
	if (sift_col_def_is_image(def))
	{
		SiftColValue lanes[SIFT_COL_MAX_LANES];		/* a range or network head also yields its lanes */

		sift_col_datum_values(def, d, false, lanes);
		*v = lanes[0];
	}
	else if (def->type == SIFT_COLT_MONEY)
		v->v.i = DatumGetInt64(d);
	else if (def->type == SIFT_COLT_INTERVAL)
		sift_col_interval_lanes(DatumGetIntervalP(d), &v->v.i, &v->lo, &md);
	else if (def->store == SIFT_COLS_STR)
		v->v.s = (const char *) sift_col_numeric_image(d, &v->len, &dscale);
	else
		return num_class(d) == NC_FINITE && wide_key(def, d, SIFT_COL_ROUND_DOWN, v) == 0 && wide_key(def, d, SIFT_COL_ROUND_UP, &up) == 0 &&
			v->v.i == up.v.i && v->lo == up.lo;
	return true;
}

static SiftColPred
pred_of(const SiftColDef *def, SiftColOp op, int nvals, SiftColValue *vals)
{
	return (SiftColPred) {def->id, op, nvals, vals};
}

static void
test_pred(SiftColWTest *t, const SiftColDef *def, SiftColOp op, int nvals, SiftColValue *vals, bool nan_passes)
{
	t->verdict = SIFT_COLW_PRED;
	t->pred = pred_of(def, op, nvals, vals);
	t->nan_passes = nan_passes;
}

/* leaf op d for a numeric of two lanes: the comparison on the scaled integers that holds for the same values (x = c needs c a multiple of the unit, x < c is x < ceil(c), x <= c is x <= floor(c), ...) */
static void
numeric_cmp_test(SiftColWTest *t, const SiftColDef *def, int op, Datum d, MemoryContext mcxt)
{
	NumClass	cls = num_class(d);
	SiftColValue *v = MemoryContextAllocZero(mcxt, sizeof(SiftColValue)),
				fl = {{0}},
				ce = {{0}};
	int			sf,
				sc,
				side;

	if (cls != NC_FINITE)
	{
		t->verdict = special_verdict[cls - NC_NAN][op];
		return;
	}
	sf = wide_key(def, d, SIFT_COL_ROUND_DOWN, &fl);
	sc = wide_key(def, d, SIFT_COL_ROUND_UP, &ce);
	if (op == 0 || op == 1)
	{
		*v = fl;
		if (sf == 0 && sc == 0 && fl.v.i == ce.v.i && fl.lo == ce.lo)
			test_pred(t, def, SIFT_COLOP_EQ + op, 1, v, op == 1);
		else
			t->verdict = op == 0 ? SIFT_COLW_NONE : SIFT_COLW_NONNULL;
		return;
	}
	side = (op == 2 || op == 5) ? sc : sf;
	if (side != 0)
		t->verdict = (op == 2 || op == 3) ? (side > 0 ? SIFT_COLW_NONNULL_NONAN : SIFT_COLW_NONE) : (side < 0 ? SIFT_COLW_NONNULL : SIFT_COLW_NAN);
	else
	{
		*v = (op == 2 || op == 5) ? ce : fl;
		test_pred(t, def, SIFT_COLOP_EQ + op, 1, v, op >= 4);
	}
}

static void
cmp_test(SiftColWTest *t, const SiftColDef *def, int op, Datum d, MemoryContext mcxt)
{
	SiftColValue *v;

	if (is_numeric_wide(def))
		return numeric_cmp_test(t, def, op, d, mcxt);
	v = MemoryContextAllocZero(mcxt, sizeof(SiftColValue));
	exact_key(def, d, v);
	test_pred(t, def, SIFT_COLOP_EQ + op, 1, v, false);
}

/* leaf IN (list) and NOT IN (list): a NULL among them removes no row from IN, and from NOT IN every row (never true); a NaN of a lane pair leaf is the NaN rows */
static void
list_test(SiftColWTest *t, const SiftColDef *def, bool notin, int nvals, const Datum *vals, const bool *isnull, MemoryContext mcxt)
{
	SiftColValue *keys = MemoryContextAllocZero(mcxt, Max(nvals, 1) * sizeof(SiftColValue));
	int			n = 0;
	bool		nan = false;

	if (nvals == 0)
	{
		t->verdict = notin ? SIFT_COLW_ALL : SIFT_COLW_NONE;
		return;
	}
	for (int i = 0; i < nvals; i++)
	{
		if (isnull[i])
		{
			if (notin)
			{
				t->verdict = SIFT_COLW_NONE;
				return;
			}
			continue;
		}
		nan |= is_numeric_wide(def) && num_class(vals[i]) == NC_NAN;
		if (exact_key(def, vals[i], &keys[n]))
			n++;
	}
	if (n == 0)
		t->verdict = notin ? (nan ? SIFT_COLW_NONNULL_NONAN : SIFT_COLW_NONNULL) : (nan ? SIFT_COLW_NAN : SIFT_COLW_NONE);
	else
		test_pred(t, def, notin ? SIFT_COLOP_NOTIN : SIFT_COLOP_IN, n, keys, notin ? !nan : nan);
}

void
sift_colw_test(const SiftColDef *def, Oid typid, int op, int nvals, const Datum *vals, const bool *isnull, MemoryContext mcxt, SiftColWTest *t)
{
	memset(t, 0, sizeof(*t));
	t->pred.col = def->id;
	if (op >= SIFT_COLW_SHAPE_FIRST)
		sift_col_shape_test(def, typid, op, vals[0], mcxt, t);
	else if (op == SIFT_COLW_ISNULL || op == SIFT_COLW_NOTNULL)
		test_pred(t, def, op == SIFT_COLW_ISNULL ? SIFT_COLOP_ISNULL : SIFT_COLOP_NOTNULL, 0, NULL, op == SIFT_COLW_NOTNULL);
	else if (op == SIFT_COLW_IN || op == SIFT_COLW_NOTIN)
		list_test(t, def, op == SIFT_COLW_NOTIN, nvals, vals, isnull, mcxt);
	else if (isnull[0])
		t->verdict = SIFT_COLW_NONE;
	else
		cmp_test(t, def, op, vals[0], mcxt);
}

/* the rows of sel that the lane col holds as overflows (a NaN of a numeric of two lanes: NULL with the flag in its lanes) */
void
sift_colw_ovf_rows(const SiftColUnit *unit, int col, const SiftColSel *sel, uint64 *out)
{
	SiftColZone z;
	SiftColVec	v;

	memset(out, 0, sel->nwords * sizeof(uint64));
	sift_col_zone(unit, col, &z);
	if (!z.has_ovf)
		return;
	sift_col_read(unit, col, NULL, &v);
	for (uint32 w = 0; v.ovf && w < sel->nwords; w++)
		out[w] = sel->bits[w] & v.ovf[w];
}

bool
sift_colw_apply(const SiftColUnit *unit, const SiftColWTest *t, SiftColSel *sel)
{
	int			col = t->pred.col;
	bool		numw = is_numeric_wide(sift_col_def(unit->src, col)),
				nanp = t->nan_passes;
	uint64	   *nan = palloc(Max(sel->nwords, 1) * sizeof(uint64));
	SiftColPred nn = {col, SIFT_COLOP_NOTNULL, 0, NULL};
	const SiftColPred *p = &t->pred;

	memset(nan, 0, Max(sel->nwords, 1) * sizeof(uint64));
	if (numw)
		sift_colw_ovf_rows(unit, col, sel, nan);
	switch (t->verdict)
	{
		case SIFT_COLW_NONE:
			memset(sel->bits, 0, sel->nwords * sizeof(uint64));
			sel->count = 0;
			return false;
		case SIFT_COLW_ALL:
			return sel->count > 0;
		case SIFT_COLW_NAN:
			memcpy(sel->bits, nan, sel->nwords * sizeof(uint64));
			return sift_col_sel_count(sel) > 0;
		case SIFT_COLW_NONNULL:
		case SIFT_COLW_NONNULL_NONAN:
			p = &nn;
			nanp = t->verdict == SIFT_COLW_NONNULL;
			break;
		case SIFT_COLW_SHAPE:
			return sift_col_shape_apply(unit, t, sel);
		case SIFT_COLW_PRED:
			break;
	}
	if (!sift_col_filter(unit, p, 1, sel))
		memset(sel->bits, 0, sel->nwords * sizeof(uint64));
	for (uint32 w = 0; w < sel->nwords; w++)
		sel->bits[w] = nanp ? (sel->bits[w] | nan[w]) : (sel->bits[w] & ~nan[w]);
	return sift_col_sel_count(sel) > 0;
}
