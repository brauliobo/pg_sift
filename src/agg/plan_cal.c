/*
 * agg/plan_cal.c - a comparison of a calendar step of a timestamp leaf with a constant is a range of the leaf.
 *
 * ts::date, date(ts), date_trunc(unit, ts) and extract(year FROM ts) change value on a step boundary and never go back, so `f(x) op C` holds for the x
 * of an interval [lo, hi) of the leaf (an equality), a half line, or the two half lines around it (<>): the leaf is compared by the kernels with the
 * zones of its units, where the expression evaluated by the executor for each distinct timestamp costs 120 to 240 ns a row.
 *
 *   lo = the first x with f(x) >= C   hi = the first x with f(x) > C   (agg/pred_cal.c: found when the scan starts, in the session's time zone)
 *   f(x) = C: lo <= x < hi (empty when C is no boundary)   >= C: x >= lo   > C: x >= hi   < C: x < lo   <= C: x < hi
 *
 * Only for a finite constant of the years 100 to 9900. The infinities of the leaf compare as the heap's function has them: date(infinity) = infinity,
 * date_trunc of it, extract(year) of it.
 */
#include "postgres.h"

#include "access/stratnum.h"
#include "catalog/pg_am_d.h"
#include "catalog/pg_type_d.h"
#include "commands/defrem.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "optimizer/optimizer.h"
#include "utils/builtins.h"
#include "utils/date.h"
#include "utils/datetime.h"
#include "utils/fmgroids.h"
#include "utils/lsyscache.h"
#include "utils/numeric.h"
#include "utils/timestamp.h"

#include "agg/plan_int.h"

#define CAL_YEAR_MIN	100
#define CAL_YEAR_MAX	9900

/* the calendar function f(x) of a timestamp or timestamptz expression: its type, the result type, x */
typedef struct CalFunc
{
	Oid			ttype;			/* TIMESTAMPOID or TIMESTAMPTZOID: the leaf's */
	bool		year;			/* extract(year) / date_part('year') */
	Oid			rtype;			/* the type of f(x) */
	Node	   *x;
} CalFunc;

static bool
cal_func(Node *e, CalFunc *f)
{
	FuncExpr   *fe;
	Oid			fid;

	if (!IsA(e, FuncExpr))
		return false;
	fe = (FuncExpr *) e;
	fid = fe->funcid;
	f->year = false;
	if (fid == F_DATE_TIMESTAMP || fid == F_DATE_TIMESTAMPTZ)
	{
		f->ttype = fid == F_DATE_TIMESTAMP ? TIMESTAMPOID : TIMESTAMPTZOID;
		f->rtype = DATEOID;
		f->x = linitial(fe->args);
		return true;
	}
	if (list_length(fe->args) != 2)
		return false;
	f->x = lsecond(fe->args);
	if (fid == F_DATE_TRUNC_TEXT_TIMESTAMP || fid == F_DATE_TRUNC_TEXT_TIMESTAMPTZ)
	{
		f->ttype = fid == F_DATE_TRUNC_TEXT_TIMESTAMP ? TIMESTAMPOID : TIMESTAMPTZOID;
		f->rtype = f->ttype;
		return bucket_unit(e, f->ttype == TIMESTAMPTZOID) != NULL;
	}
	if (fid == F_EXTRACT_TEXT_TIMESTAMP || fid == F_EXTRACT_TEXT_TIMESTAMPTZ || fid == F_DATE_PART_TEXT_TIMESTAMP || fid == F_DATE_PART_TEXT_TIMESTAMPTZ)
	{
		const BucketUnit *u;

		f->ttype = fid == F_EXTRACT_TEXT_TIMESTAMP || fid == F_DATE_PART_TEXT_TIMESTAMP ? TIMESTAMPOID : TIMESTAMPTZOID;
		f->rtype = fid == F_EXTRACT_TEXT_TIMESTAMP || fid == F_EXTRACT_TEXT_TIMESTAMPTZ ? NUMERICOID : FLOAT8OID;
		u = bucket_unit(e, f->ttype == TIMESTAMPTZOID);
		f->year = u != NULL && strcmp(u->name, "year") == 0;
		return f->year;
	}
	return false;
}

/* the constant is finite and in the years CAL_YEAR_MIN to CAL_YEAR_MAX; for a year, a whole number */
static bool
cal_const_ok(const CalFunc *f, Const *k)
{
	Datum		d = k->constvalue;

	if (k->constisnull)
		return false;
	if (f->year)
	{
		double		y = f->rtype == FLOAT8OID ? DatumGetFloat8(d) : DatumGetFloat8(DirectFunctionCall1(numeric_float8, d));

		return y >= CAL_YEAR_MIN && y <= CAL_YEAR_MAX && y == floor(y);
	}
	if (f->rtype == DATEOID)
	{
		DateADT		dt = DatumGetDateADT(d);

		return !DATE_NOT_FINITE(dt) && dt >= date2j(CAL_YEAR_MIN, 1, 1) - POSTGRES_EPOCH_JDATE && dt <= date2j(CAL_YEAR_MAX, 1, 1) - POSTGRES_EPOCH_JDATE;
	}
	{
		Timestamp	t = DatumGetTimestamp(d);

		return !TIMESTAMP_NOT_FINITE(t) && t >= (int64) (date2j(CAL_YEAR_MIN, 1, 1) - POSTGRES_EPOCH_JDATE) * USECS_PER_DAY &&
			t <= (int64) (date2j(CAL_YEAR_MAX, 1, 1) - POSTGRES_EPOCH_JDATE) * USECS_PER_DAY;
	}
}

/* the operand `mode` of the search `cal`: its value at plan time (in the time zone of the planning session) is for the estimates and the zone ranges */
static Node *
bound_const(Oid ttype, Node *cal, int mode)
{
	return (Node *) makeConst(ttype, -1, InvalidOid, sizeof(int64), sift_cal_bound(cal, mode), false, true);
}

static SiftAggPred *
bound_pred(AggCtx *c, int leaf, Oid ttype, int op, Node *cal, int mode, Node *bound, double sel)
{
	SiftAggPred *p = make_leaf_pred(c, SAP_CMP, leaf);
	FuncExpr   *f = linitial(castNode(List, cal));

	p->op = op;
	p->cal = copyObject(cal);
	p->calmode = mode;
	p->expr = bound;
	p->argtype = ttype;
	p->sel = sel;
	p->desc = psprintf("%s %s first(%s %s %s)", leaf_name(c, leaf), cmp_text(op), expr_text(c, (Node *) f), mode == 1 ? ">=" : ">", expr_text(c, lsecond(castNode(List, cal))));
	return p;
}

/* the selectivity of `x op bound` from the statistics of the leaf's column */
static double
bound_sel(AggCtx *c, Node *x, Oid ttype, int strategy, Node *bound)
{
	Oid			opc = GetDefaultOpClass(ttype, BTREE_AM_OID),
				opno = OidIsValid(opc) ? get_opfamily_member(get_opclass_family(opc), ttype, ttype, strategy) : InvalidOid;
	OpExpr	   *e;

	if (!OidIsValid(opno))
		return 1.0;
	e = (OpExpr *) make_opclause(opno, BOOLOID, false, (Expr *) copyObject(x), (Expr *) bound, InvalidOid, InvalidOid);
	set_opfuncid(e);
	return clause_selectivity(c->root, (Node *) e, c->relid, JOIN_INNER, NULL);
}

SiftAggPred *
compile_calendar_cmp(AggCtx *c, OpExpr *op)
{
	Oid			lt,
				rt;
	int			cmp = builtin_cmp(op->opno, &lt, &rt),
				leaf;
	Node	   *l,
			   *r;
	CalFunc		f;
	FuncExpr   *fe;
	Oid			t;
	Node	   *cal;

	if (cmp < 0 || list_length(op->args) != 2 || lt != rt)
		return NULL;
	l = strip_relabel(linitial(op->args));
	r = strip_relabel(lsecond(op->args));
	if (!IsA(r, Const))
	{
		Node	   *tmp = l;

		l = r;
		r = tmp;
		cmp = commute(cmp);
	}
	if (!IsA(r, Const) || !cal_func(l, &f) || f.rtype != lt || !cal_const_ok(&f, (Const *) r) || (leaf = match_leaf(c, strip_relabel(f.x))) < 0)
		return NULL;
	{
		const SiftSrcLeaf *lf = leaf_of(c, leaf);

		if (lf->typid != f.ttype || lf->kind != SCL_SCALAR || lf->vtype != SCV_I64 || lf->lossy || lf->parent >= 0)
			return NULL;
	}
	t = f.ttype;
	/* f(placeholder) and C: the search of agg/pred_cal.c evaluates f at the timestamps it tries */
	fe = copyObject((FuncExpr *) l);
	llast(fe->args) = makeConst(t, -1, InvalidOid, sizeof(int64), TimestampGetDatum(0), false, true);
	cal = (Node *) list_make2(fe, copyObject(r));
	switch (cmp)
	{
		case SCMP_EQ:
		case SCMP_NE:
			{
				Node	   *lo_v = bound_const(t, cal, 1),
						   *hi_v = bound_const(t, cal, 2);
				double		s_lo = bound_sel(c, f.x, t, BTGreaterEqualStrategyNumber, lo_v),
							s_hi = bound_sel(c, f.x, t, BTLessStrategyNumber, hi_v);
				SiftAggPred *p = new_junction(c, cmp == SCMP_EQ ? SAP_AND : SAP_OR, 2);

				/* the selectivity of the range is in the first child of the AND; the second adds no condition */
				p->child[0] = bound_pred(c, leaf, t, cmp == SCMP_EQ ? SCMP_GE : SCMP_LT, cal, 1, lo_v, cmp == SCMP_EQ ? Min(1.0, Max(0.0, s_lo + s_hi - 1.0)) : 1 - s_lo);
				p->child[1] = bound_pred(c, leaf, t, cmp == SCMP_EQ ? SCMP_LT : SCMP_GE, cal, 2, hi_v, cmp == SCMP_EQ ? 1.0 : 1 - s_hi);
				junction_done(p);
				return p;
			}
		default:
			{
				bool		ge = cmp == SCMP_GE || cmp == SCMP_GT;
				int			mode = cmp == SCMP_GE || cmp == SCMP_LT ? 1 : 2;
				Node	   *v = bound_const(t, cal, mode);

				return bound_pred(c, leaf, t, ge ? SCMP_GE : SCMP_LT, cal, mode, v, bound_sel(c, f.x, t, ge ? BTGreaterEqualStrategyNumber : BTLessStrategyNumber, v));
			}
	}
}
