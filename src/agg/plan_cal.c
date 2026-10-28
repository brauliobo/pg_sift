/*
 * agg/plan_cal.c - a comparison of a calendar step of a timestamp leaf with a constant is a range of the leaf.
 *
 * ts::date, date(ts), date_trunc(unit, ts) and extract(year FROM ts) change value on a step boundary and never go back, so `f(x) op C` holds for the x
 * of an interval [lo, hi) of the leaf (an equality), a half line, or the two half lines around it (<>): the leaf is compared by the kernels with the
 * zones of its units, where the expression evaluated by the executor for each distinct timestamp costs 120 to 240 ns a row.
 *
 *   lo = the first x with f(x) >= C   hi = the last x with f(x) <= C   (agg/pred_cal.c: found when the scan starts, in the session's time zone)
 *   f(x) = C: lo <= x <= hi (empty when C is no boundary)   >= C: x >= lo   > C: x > hi   < C: x < lo   <= C: x <= hi
 *
 * The operand is a constant, a stable expression (current_date - 30, now() - interval) or a parameter, evaluated when the scan starts; one beyond the timestamps
 * saturates (agg/pred_cal.c). The infinities of the leaf compare as the heap's function has them: date(infinity) = infinity, date_trunc of it, extract(year) of it.
 */
#include "postgres.h"

#include "access/stratnum.h"
#include "access/nbtree.h"
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

/* the calendar function f(x) of a timestamp or timestamptz expression: its type, the result type, x */
typedef struct CalFunc
{
	Oid			ttype;			/* TIMESTAMPOID or TIMESTAMPTZOID: the leaf's */
	bool		year;			/* extract(year) / date_part('year') */
	bool		bare;			/* no call: the leaf itself (a comparison of two types) */
	Oid			rtype;			/* the type of f(x) */
	Node	   *x;
} CalFunc;

static bool
cal_func(Node *e, CalFunc *f)
{
	FuncExpr   *fe;
	Oid			fid;

	f->bare = false;
	f->year = false;
	if (!IsA(e, FuncExpr))
	{
		f->ttype = f->rtype = exprType(e);
		f->x = e;
		f->bare = true;
		return f->ttype == TIMESTAMPOID || f->ttype == TIMESTAMPTZOID;
	}
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

static SiftAggPred *
bound_pred(AggCtx *c, int leaf, Oid ttype, int op, FuncExpr *f, int mode, Node *operand, double sel)
{
	SiftAggPred *p = make_leaf_pred(c, SAP_CMP, leaf);

	p->op = op;
	p->cal = (Node *) f;
	p->calmode = mode;
	p->expr = operand;
	p->argtype = ttype;
	p->sel = sel;
	p->desc = psprintf("%s %s %s(%s %s %s)", leaf_name(c, leaf), cmp_text(op), mode == 1 ? "first" : "last", expr_text(c, (Node *) f), mode == 1 ? ">=" : "<=", expr_text(c, operand));
	return p;
}

/* the selectivity of `x op bound` from the statistics of the leaf's column, the bound the search finds for the operand's value at plan time (in the planning session's time zone) */
static double
bound_sel(AggCtx *c, Node *x, Oid ttype, int cmp, FuncExpr *f, int mode, Node *operand)
{
	int			strategy = cmp == SCMP_GE ? BTGreaterEqualStrategyNumber : cmp == SCMP_GT ? BTGreaterStrategyNumber : cmp == SCMP_LT ? BTLessStrategyNumber : BTLessEqualStrategyNumber;
	Node	   *bound = (Node *) makeConst(ttype, -1, InvalidOid, sizeof(int64), sift_cal_bound((Node *) f, mode, ((Const *) operand)->constvalue), false, true);
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
			   *r,
			   *folded;
	CalFunc		f;
	FuncExpr   *fe;
	Oid			t;
	double		whole;
	bool		known;

	if (cmp < 0 || list_length(op->args) != 2)
		return NULL;
	l = strip_relabel(linitial(op->args));
	r = strip_relabel(lsecond(op->args));
	if (!pseudo_const(r))
	{
		Node	   *tmp = l;

		l = r;
		r = tmp;
		cmp = commute(cmp);
		t = lt;
		lt = rt;
		rt = t;
	}
	if (!pseudo_const(r) || !cal_func(l, &f) || f.rtype != lt || (f.bare && lt == rt) || (leaf = match_leaf(c, strip_relabel(f.x))) < 0)
		return NULL;
	/* the value of a statement planned with it (a constant, now(), current_date) prices the scan; a parameter of a generic plan has the estimate of the whole clause */
	folded = estimate_expression_value(c->root, r);
	known = IsA(folded, Const) && !((Const *) folded)->constisnull;
	/* a year is a number: a NaN operand (known only as a constant of the plan) compares above +infinity, and so does nothing a timestamp bound says */
	if (f.year && (!IsA(folded, Const) || (!((Const *) folded)->constisnull && sift_cal_nan(rt, ((Const *) folded)->constvalue))))
		return NULL;
	{
		const SiftSrcLeaf *lf = leaf_of(c, leaf);

		if (lf->typid != f.ttype || lf->kind != SCL_SCALAR || lf->vtype != SCV_I64 || lf->lossy || lf->parent >= 0)
			return NULL;
	}
	t = f.ttype;
	/* f(placeholder), or the b-tree comparison of two types with it as its first argument: the search of agg/pred_cal.c evaluates it at the timestamps it tries */
	fe = f.bare ? NULL : copyObject((FuncExpr *) l);
	if (fe)
		llast(fe->args) = makeConst(t, -1, InvalidOid, sizeof(int64), TimestampGetDatum(0), false, true);
	if (lt != rt)
	{
		Oid			opc = GetDefaultOpClass(t, BTREE_AM_OID),
					proc = OidIsValid(opc) ? get_opfamily_proc(get_opclass_family(opc), lt, rt, BTORDER_PROC) : InvalidOid;
		Node	   *first = fe ? (Node *) fe : (Node *) makeConst(t, -1, InvalidOid, sizeof(int64), TimestampGetDatum(0), false, true);

		if (!OidIsValid(proc))
			return NULL;
		fe = makeFuncExpr(proc, INT4OID, list_make2(first, makeConst(rt, -1, InvalidOid, 0, (Datum) 0, true, true)), InvalidOid, InvalidOid, COERCE_EXPLICIT_CALL);
	}
	whole = clause_selectivity(c->root, (Node *) op, c->relid, JOIN_INNER, NULL);
	switch (cmp)
	{
		case SCMP_EQ:
		case SCMP_NE:
			{
				double		s_lo = whole,
							s_hi = whole;
				SiftAggPred *p = new_junction(c, cmp == SCMP_EQ ? SAP_AND : SAP_OR, 2);

				if (known)
				{
					s_lo = bound_sel(c, f.x, t, SCMP_GE, fe, 1, folded);
					s_hi = bound_sel(c, f.x, t, SCMP_LE, fe, 2, folded);
				}
				/* the selectivity of the range is in the first child of the AND; the second adds no condition */
				p->child[0] = bound_pred(c, leaf, t, cmp == SCMP_EQ ? SCMP_GE : SCMP_LT, fe, 1, r, !known ? whole : cmp == SCMP_EQ ? Min(1.0, Max(0.0, s_lo + s_hi - 1.0)) : 1 - s_lo);
				p->child[1] = bound_pred(c, leaf, t, cmp == SCMP_EQ ? SCMP_LE : SCMP_GT, fe, 2, r, !known ? (cmp == SCMP_EQ ? 1.0 : 0.0) : cmp == SCMP_EQ ? 1.0 : 1 - s_hi);
				junction_done(p);
				return p;
			}
		default:
			/* >= and < compare with the first x that reaches the operand, > and <= with the last x that does not pass it */
			return bound_pred(c, leaf, t, cmp, fe, cmp == SCMP_GE || cmp == SCMP_LT ? 1 : 2, r, known ? bound_sel(c, f.x, t, cmp, fe, cmp == SCMP_GE || cmp == SCMP_LT ? 1 : 2, folded) : whole);
	}
}

/*
 * An equality of a periodic calendar step with a constant (extract(hour FROM ts) = 14, extract(month ..) = 3, doy, day, minute, and to_char of
 * the numeric fields YYYY, MM, DD, HH24, MI, SS in that order) on a timestamp leaf is not a range of the leaf, but inside a window (the day of the hour, the year of the month
 * ..) the step never goes back, so its equal values are one interval of the window (sift_cal_periodic): the ranges of the windows of the leaf's span are a prefilter the
 * source applies before the expression runs (init_filter.c). The predicate stays what it was (a lookup table or a combo): the prefilter may keep more rows than the answer;
 * cal is the List (f(placeholder), operand, leaf).
 */
typedef struct PeriodField
{
	const char *name;
	int			win;
} PeriodField;

static const PeriodField period_fields[] = {{"minute", SCW_HOUR}, {"hour", SCW_DAY}, {"day", SCW_MONTH}, {"doy", SCW_YEAR}, {"month", SCW_YEAR}, {NULL, 0}};

/* to_char formats made of the numeric fields in order of size, so that the text of a window is in order: the window of the first one (YYYY: SCW_NONE | SCW_YEAR4) */
static int
to_char_window(const char *fmt)
{
	static const char *const tok[] = {"YYYY", "MM", "DD", "HH24", "MI", "SS"};
	static const int win[] = {SCW_NONE | SCW_YEAR4, SCW_YEAR, SCW_MONTH, SCW_DAY, SCW_HOUR, SCW_MINUTE};
	int			last = -1,
				first = -1;

	for (const char *q = fmt; *q;)
	{
		int			t;

		for (t = 0; t < 6; t++)
			if (strncmp(q, tok[t], strlen(tok[t])) == 0)
				break;
		if (t < 6)
		{
			if (t <= last)
				return -1;
			if (first < 0)
				first = t;
			last = t;
			q += strlen(tok[t]);
		}
		else if (strchr("-/:. ", *q) != NULL)
			q++;
		else
			return -1;
	}
	return first < 0 ? -1 : win[first];
}

/* the window of f, and f(placeholder); false when f is no periodic step. The weekday and the quarter are not tried: an eighth of the values is the most a prefilter keeps, and they are a seventh and a fourth */
static bool
period_func(Node *e, Oid *ttype, int *win, FuncExpr **fcopy, Node **x)
{
	FuncExpr   *fe;
	Oid			fid;
	Const	   *k;
	bool		tz;

	if (!IsA(e, FuncExpr))
		return false;
	fe = (FuncExpr *) e;
	fid = fe->funcid;
	if (fid == F_TO_CHAR_TIMESTAMP_TEXT || fid == F_TO_CHAR_TIMESTAMPTZ_TEXT)
	{
		k = (Const *) lsecond(fe->args);
		*ttype = fid == F_TO_CHAR_TIMESTAMP_TEXT ? TIMESTAMPOID : TIMESTAMPTZOID;
		if (!IsA(k, Const) || k->constisnull || (*win = to_char_window(TextDatumGetCString(k->constvalue))) < 0)
			return false;
		*x = linitial(fe->args);
		*fcopy = copyObject(fe);
		linitial((*fcopy)->args) = makeConst(*ttype, -1, InvalidOid, sizeof(int64), TimestampGetDatum(0), false, true);
		return true;
	}
	if (fid != F_EXTRACT_TEXT_TIMESTAMP && fid != F_EXTRACT_TEXT_TIMESTAMPTZ && fid != F_DATE_PART_TEXT_TIMESTAMP && fid != F_DATE_PART_TEXT_TIMESTAMPTZ)
		return false;
	tz = fid == F_EXTRACT_TEXT_TIMESTAMPTZ || fid == F_DATE_PART_TEXT_TIMESTAMPTZ;
	*ttype = tz ? TIMESTAMPTZOID : TIMESTAMPOID;
	k = (Const *) linitial(fe->args);
	if (bucket_unit(e, tz) == NULL)
		return false;
	for (const PeriodField *pf = period_fields; pf->name; pf++)
		if (pg_strcasecmp(TextDatumGetCString(k->constvalue), pf->name) == 0)
		{
			*win = pf->win;
			*x = llast(fe->args);
			*fcopy = copyObject(fe);
			llast((*fcopy)->args) = makeConst(*ttype, -1, InvalidOid, sizeof(int64), TimestampGetDatum(0), false, true);
			return true;
		}
	return false;
}

void
compile_periodic(AggCtx *c, Node *n, SiftAggPred *p)
{
	OpExpr	   *op;
	Oid			lt,
				rt,
				ttype;
	Node	   *l,
			   *r;
	FuncExpr   *f;
	Node	   *x;
	int			win,
				leaf;
	int64		lo,
				hi;
	const SiftSrcLeaf *lf;

	if (!IsA(n, OpExpr) || (p->kind != SAP_LUT && p->kind != SAP_COMBO))
		return;
	op = (OpExpr *) n;
	if (builtin_cmp(op->opno, &lt, &rt) != SCMP_EQ || list_length(op->args) != 2 || (op->inputcollid != InvalidOid && !get_collation_isdeterministic(op->inputcollid)))
		return;
	l = strip_relabel(linitial(op->args));
	r = strip_relabel(lsecond(op->args));
	if (!pseudo_const(r))
	{
		Node	   *tmp = l;

		l = r;
		r = tmp;
	}
	if (!pseudo_const(r) || !period_func(l, &ttype, &win, &f, &x) || exprType(r) != f->funcresulttype)
		return;
	leaf = match_leaf(c, strip_relabel(x));
	if (leaf < 0)
		return;
	lf = leaf_of(c, leaf);
	if (lf->typid != ttype || lf->kind != SCL_SCALAR || lf->vtype != SCV_I64 || lf->lossy || lf->parent >= 0)
		return;
	/* more windows than the scan enumerates, by the statistics of the column: not tried (each try costs a read of the zones of the index) */
	if (leaf_stats_range(c, leaf, &lo, &hi) && sift_cal_windows(win, lo, hi) > SCW_MAXWIN)
		return;
	p->cal = (Node *) list_make3(f, r, makeInteger(leaf));
	p->calmode = win;
}
