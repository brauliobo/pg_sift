/*
 * agg/pred_cal.c - the operand of a comparison of a calendar step (agg/plan_cal.c), found when the scan starts
 *
 * The comparison is `f(x) op C`; its operand is the first timestamp whose f(x) reaches C (>= C, or > C). f is PostgreSQL's own function (::date, date_trunc,
 * extract(year)): it never goes back as x grows, whatever the session's time zone does with the clock (a gap at midnight, an hour or half an hour that
 * repeats, a day that is skipped), so a bisection over x with f itself finds the boundary exactly, in about 60 calls.
 */
#include "postgres.h"

#include "catalog/pg_type_d.h"
#include "fmgr.h"
#include "nodes/pg_list.h"
#include "nodes/primnodes.h"
#include "utils/builtins.h"
#include "utils/date.h"
#include "utils/datetime.h"
#include "utils/numeric.h"
#include "utils/timestamp.h"

#include "agg/spec.h"

/* f(x) < 0, = 0, > 0 against C. The last argument of the call is the placeholder of x */
static int
cal_cmp(FuncExpr *f, Const *c, Timestamp x)
{
	Datum		r;

	if (list_length(f->args) == 1)
		r = OidFunctionCall1Coll(f->funcid, InvalidOid, TimestampGetDatum(x));
	else
		r = OidFunctionCall2Coll(f->funcid, InvalidOid, ((Const *) linitial(f->args))->constvalue, TimestampGetDatum(x));
	switch (c->consttype)
	{
		case DATEOID:
			return (DatumGetDateADT(r) > DatumGetDateADT(c->constvalue)) - (DatumGetDateADT(r) < DatumGetDateADT(c->constvalue));
		case TIMESTAMPOID:
		case TIMESTAMPTZOID:
			return (DatumGetTimestamp(r) > DatumGetTimestamp(c->constvalue)) - (DatumGetTimestamp(r) < DatumGetTimestamp(c->constvalue));
		case NUMERICOID:
			return DatumGetInt32(DirectFunctionCall2(numeric_cmp, r, c->constvalue));
		default:
			return (DatumGetFloat8(r) > DatumGetFloat8(c->constvalue)) - (DatumGetFloat8(r) < DatumGetFloat8(c->constvalue));
	}
}

/* cal is the list (f(placeholder), C); mode 1: the least x with f(x) >= C, 2: with f(x) > C; between the years 20 and 20000, where the constants of plan_cal.c are inside */
Datum
sift_cal_bound(Node *cal, int mode)
{
	FuncExpr   *f = linitial(castNode(List, cal));
	Const	   *c = lsecond(castNode(List, cal));
	Timestamp	lo = (Timestamp) (date2j(20, 1, 1) - POSTGRES_EPOCH_JDATE) * USECS_PER_DAY,
				hi = (Timestamp) (date2j(20000, 1, 1) - POSTGRES_EPOCH_JDATE) * USECS_PER_DAY;

	while (hi - lo > 1)
	{
		Timestamp	mid = lo + (hi - lo) / 2;
		int			r = cal_cmp(f, c, mid);

		if (mode == 1 ? r >= 0 : r > 0)
			hi = mid;
		else
			lo = mid;
	}
	return TimestampGetDatum(hi);
}
