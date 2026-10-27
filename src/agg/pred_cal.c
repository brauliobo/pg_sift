/*
 * agg/pred_cal.c - the operand of a comparison of a calendar step (agg/plan_cal.c), found when the scan starts
 *
 * The comparison is `f(x) op C`; its operand is the first timestamp whose f(x) reaches C (>= C, or > C). f is PostgreSQL's own function (::date, date_trunc,
 * extract(year)): it never goes back as x grows, whatever the session's time zone does with the clock (a gap at midnight, an hour or half an hour that
 * repeats, a day that is skipped), so a bisection over x with f itself finds the boundary exactly, in about 60 calls.
 */
#include "postgres.h"

#include <math.h>

#include "catalog/pg_type_d.h"
#include "fmgr.h"
#include "nodes/nodeFuncs.h"
#include "nodes/pg_list.h"
#include "nodes/primnodes.h"
#include "utils/builtins.h"
#include "utils/date.h"
#include "utils/datetime.h"
#include "utils/numeric.h"
#include "utils/timestamp.h"

#include "agg/spec.h"

/* e at x: the placeholder (a Const) is x itself, a call of date, date_trunc, extract or date_part has the placeholder as its last argument */
static Datum
cal_eval(Node *e, Timestamp x)
{
	FuncExpr   *f;

	if (IsA(e, Const))
		return TimestampGetDatum(x);
	f = castNode(FuncExpr, e);
	if (list_length(f->args) == 1)
		return OidFunctionCall1Coll(f->funcid, InvalidOid, TimestampGetDatum(x));
	return OidFunctionCall2Coll(f->funcid, InvalidOid, ((Const *) linitial(f->args))->constvalue, TimestampGetDatum(x));
}

/*
 * The comparison of the search as a FuncExpr: a b-tree comparison function (int4 result) of two types, (e, operand), or the step f itself against an operand of
 * its own type. Its sign is that of e(x) against the operand c.
 */
static Oid
cal_operand_type(FuncExpr *f)
{
	return f->funcresulttype == INT4OID ? exprType(lsecond(f->args)) : f->funcresulttype;
}

static int
cal_cmp(FuncExpr *f, Datum c, Timestamp x)
{
	Datum		r;

	if (f->funcresulttype == INT4OID)
	{
		int32		v = DatumGetInt32(OidFunctionCall2Coll(f->funcid, InvalidOid, cal_eval(linitial(f->args), x), c));

		return (v > 0) - (v < 0);
	}
	r = cal_eval((Node *) f, x);
	switch (f->funcresulttype)
	{
		case DATEOID:
			return (DatumGetDateADT(r) > DatumGetDateADT(c)) - (DatumGetDateADT(r) < DatumGetDateADT(c));
		case TIMESTAMPOID:
		case TIMESTAMPTZOID:
			return (DatumGetTimestamp(r) > DatumGetTimestamp(c)) - (DatumGetTimestamp(r) < DatumGetTimestamp(c));
		case NUMERICOID:
			return DatumGetInt32(DirectFunctionCall2(numeric_cmp, r, c));
		default:
			return (DatumGetFloat8(r) > DatumGetFloat8(c)) - (DatumGetFloat8(r) < DatumGetFloat8(c));
	}
}

/* the part of the timestamp range at each end that the search does not evaluate f in: a millennium and a day */
#define CAL_MARGIN	(1001 * 366 * USECS_PER_DAY)

/* the sign of an operand that is infinite: -1, +1, else 0 (a NaN year is refused by the planner: it compares above +infinity, which no timestamp bound says) */
static int
cal_infinite(Oid rtype, Datum d)
{
	double		y;

	switch (rtype)
	{
		case DATEOID:
			return DATE_IS_NOBEGIN(DatumGetDateADT(d)) ? -1 : DATE_IS_NOEND(DatumGetDateADT(d)) ? 1 : 0;
		case TIMESTAMPOID:
		case TIMESTAMPTZOID:
			return TIMESTAMP_IS_NOBEGIN(DatumGetTimestamp(d)) ? -1 : TIMESTAMP_IS_NOEND(DatumGetTimestamp(d)) ? 1 : 0;
		default:
			y = rtype == FLOAT8OID ? DatumGetFloat8(d) : DatumGetFloat8(DirectFunctionCall1(numeric_float8, d));
			return isinf(y) ? (y < 0 ? -1 : 1) : 0;
	}
}

bool
sift_cal_nan(Oid rtype, Datum d)
{
	return rtype == NUMERICOID ? numeric_is_nan(DatumGetNumeric(d)) : rtype == FLOAT8OID && isnan(DatumGetFloat8(d));
}

/*
 * The timestamp bound of the comparison `f(x) op C` for the operand C (f is the call or the b-tree comparison of two types in cal):
 *   mode 1: the least x with f(x) >= C  (x >= it is f(x) >= C, x < it is f(x) < C)
 *   mode 2: the greatest x with f(x) <= C  (x <= it is f(x) <= C, x > it is f(x) > C)
 * f never goes back as x grows, so the bound is a bisection with f itself, in the session's time zone, between the two ends of the timestamp range less CAL_MARGIN
 * (date_trunc to a millennium of a timestamp near an end is out of the range, and so is a time zone conversion). An operand beyond f of an end saturates to what the heap's answer is: below every timestamp
 * (or -infinity) all the rows pass `>=` and none `<`; above, none pass `>=` and all pass `<`; the infinite timestamps of the leaf stay on their own side of any
 * finite operand, and equal to an infinite one. Exact but for an operand within the margin of an end of the range (before the year 3712 BC or after the year 292000).
 */
Datum
sift_cal_bound(Node *cal, int mode, Datum operand)
{
	FuncExpr   *f = castNode(FuncExpr, cal);
	int			inf = cal_infinite(cal_operand_type(f), operand);
	Timestamp	lo = MIN_TIMESTAMP + CAL_MARGIN,
				hi = END_TIMESTAMP - CAL_MARGIN;

	if (inf)
		return TimestampGetDatum(inf < 0 ? DT_NOBEGIN : DT_NOEND);
	if (mode == 1)
	{
		if (cal_cmp(f, operand, lo) >= 0)
			return TimestampGetDatum(MIN_TIMESTAMP);
		if (cal_cmp(f, operand, hi) < 0)
			return TimestampGetDatum(DT_NOEND);
	}
	else
	{
		if (cal_cmp(f, operand, lo) > 0)
			return TimestampGetDatum(DT_NOBEGIN);
		if (cal_cmp(f, operand, hi) <= 0)
			return TimestampGetDatum(END_TIMESTAMP - 1);
	}
	/* f(lo) is below the operand and f(hi) above it (mode 1: at or above, 2: above): the first x that reaches it is in between */
	while ((uint64) hi - (uint64) lo > 1)	/* the span of the range is above INT64_MAX */
	{
		Timestamp	mid = lo + (Timestamp) (((uint64) hi - (uint64) lo) / 2);
		int			r = cal_cmp(f, operand, mid);

		if (mode == 1 ? r >= 0 : r > 0)
			hi = mid;
		else
			lo = mid;
	}
	return TimestampGetDatum(mode == 1 ? hi : lo);
}
