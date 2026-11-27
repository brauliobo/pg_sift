/*
 * agg/periodic.c - an equality of a periodic calendar step as the ranges of its column (v6_vexpr.h sift_v6_ranges_of): extract(month|doy|day|hour|minute FROM x) = C, and to_char of x with the
 * numeric fields YYYY, MM, DD, HH24, MI, SS in that order, of a timestamp column. The step is not a range of x, but inside a window (the year of the month, the day of the hour ..) it never goes
 * back, so its equal values are one interval of the window, found by two bisections with PostgreSQL's own function in the session's zone. The windows of the span the statistics of the index
 * give, and the values before and after that span, are the ranges; the quals still test every row of the granules that are read.
 */
#include "postgres.h"

#include "access/stratnum.h"
#include "catalog/pg_am_d.h"
#include "catalog/pg_type_d.h"
#include "commands/defrem.h"
#include "datatype/timestamp.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "pgtime.h"
#include "utils/builtins.h"
#include "utils/datetime.h"
#include "utils/fmgroids.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/timestamp.h"

#include "agg/ht/ht.h"
#include "agg/vexpr.h"

typedef enum Window
{
	WIN_YEAR = 1,
	WIN_MONTH,
	WIN_DAY,
	WIN_HOUR,
	WIN_MINUTE
} Window;

#define PERIODIC_MAX_WINDOWS	256		/* the most windows the ranges enumerate */
#define PERIODIC_SHARE			8		/* the ranges keep at most an eighth of the span: the weekday and the quarter (a seventh, a fourth) are not tried */
#define PERIODIC_MAX_CUTS		4096

typedef struct Step
{
	FuncExpr   *f;				/* the step with its column replaced by a placeholder */
	Oid			ttype;
	Window		win;
	bool		year4;			/* to_char starts with YYYY: only the years 1000 to 9999 are in order as text */
	Datum		operand;
} Step;

typedef struct PeriodField
{
	const char *name;
	Window		win;
} PeriodField;

static const PeriodField period_fields[] = {{"minute", WIN_HOUR}, {"hour", WIN_DAY}, {"day", WIN_MONTH}, {"doy", WIN_YEAR}, {"month", WIN_YEAR}, {NULL, 0}};

/* to_char formats made of the numeric fields in order of size, so that the text of a window is in order: the window of the first one, 0 for the whole span (YYYY), -1 for no such format */
static int
to_char_window(const char *fmt, bool *year4)
{
	static const char *const tok[] = {"YYYY", "MM", "DD", "HH24", "MI", "SS"};
	static const int win[] = {0, WIN_YEAR, WIN_MONTH, WIN_DAY, WIN_HOUR, WIN_MINUTE};
	int			last = -1,
				first = -1;

	for (const char *q = fmt; *q;)
	{
		int			t;

		for (t = 0; t < 6 && strncmp(q, tok[t], strlen(tok[t])) != 0; t++);
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
	*year4 = first == 0;
	return first < 0 ? -1 : win[first];
}

/* the column of the step (a timestamp Var of relid) and the step with a placeholder in its place; false for any other call */
static bool
step_of(const Node *e, Index relid, Step *s, const Var **var)
{
	const FuncExpr *fe = (const FuncExpr *) e;
	const Const *k;
	int			x;

	if (!IsA(e, FuncExpr) || list_length(fe->args) != 2)
		return false;
	switch (fe->funcid)
	{
		case F_TO_CHAR_TIMESTAMP_TEXT:
		case F_TO_CHAR_TIMESTAMPTZ_TEXT:
			k = lsecond(fe->args);
			x = 0;
			s->ttype = fe->funcid == F_TO_CHAR_TIMESTAMP_TEXT ? TIMESTAMPOID : TIMESTAMPTZOID;
			if (!IsA(k, Const) || k->constisnull || (int) (s->win = to_char_window(TextDatumGetCString(k->constvalue), &s->year4)) < 0)
				return false;
			break;
		case F_EXTRACT_TEXT_TIMESTAMP:
		case F_EXTRACT_TEXT_TIMESTAMPTZ:
		case F_DATE_PART_TEXT_TIMESTAMP:
		case F_DATE_PART_TEXT_TIMESTAMPTZ:
			k = linitial(fe->args);
			x = 1;
			s->ttype = fe->funcid == F_EXTRACT_TEXT_TIMESTAMP || fe->funcid == F_DATE_PART_TEXT_TIMESTAMP ? TIMESTAMPOID : TIMESTAMPTZOID;
			if (!IsA(k, Const) || k->constisnull)
				return false;
			s->win = 0;
			for (const PeriodField *pf = period_fields; pf->name != NULL && s->win == 0; pf++)
				if (pg_strcasecmp(TextDatumGetCString(k->constvalue), pf->name) == 0)
					s->win = pf->win;
			if (s->win == 0)
				return false;
			break;
		default:
			return false;
	}
	*var = list_nth(fe->args, x);
	if (!IsA(*var, Var) || (*var)->varno != relid || (*var)->varlevelsup != 0 || (*var)->vartype != s->ttype)
		return false;
	s->f = (FuncExpr *) copyObject(fe);
	list_nth_cell(s->f->args, x)->ptr_value = makeConst(s->ttype, -1, InvalidOid, sizeof(int64), TimestampGetDatum(0), false, FLOAT8PASSBYVAL);
	return true;
}

/* the operand: a constant or the value this execution gives a parameter, of the type of the step; false when it is neither or NULL */
static bool
operand_of(const Node *e, Oid type, ParamListInfo params, Datum *d)
{
	bool		isnull;

	if (IsA(e, Const))
	{
		*d = ((const Const *) e)->constvalue;
		isnull = ((const Const *) e)->constisnull;
	}
	else if (!IsA(e, Param) || !sift_v6_param_value(params, (const Param *) e, d, &isnull))
		return false;
	return exprType(e) == type && !isnull;
}

/* `step = operand` with the equality of the default b-tree class of the step's type (a text one of a collation that compares bytes) */
static bool
equality_of(const Node *clause, Index relid, ParamListInfo params, Step *s, const Var **var)
{
	const OpExpr *op = (const OpExpr *) clause;
	const Node *l,
			   *r;
	Oid			t;

	if (!IsA(clause, OpExpr) || list_length(op->args) != 2)
		return false;
	l = linitial(op->args);
	r = lsecond(op->args);
	if (!IsA(l, FuncExpr))
	{
		const Node *tmp = l;

		l = r;
		r = tmp;
	}
	if (!step_of(l, relid, s, var))
		return false;
	t = s->f->funcresulttype;
	if ((t == TEXTOID && !sift_text_bytes_equal(op->inputcollid)) ||
		get_op_opfamily_strategy(op->opno, get_opclass_family(GetDefaultOpClass(t, BTREE_AM_OID))) != BTEqualStrategyNumber)
		return false;
	return operand_of(r, t, params, &s->operand);
}

/* the sign of f(x) against the operand */
static int
step_cmp(const Step *s, Timestamp x)
{
	FuncExpr   *f = s->f;
	const bool	is_text = f->funcresulttype == TEXTOID;
	Datum		r = is_text ? OidFunctionCall2(f->funcid, TimestampGetDatum(x), ((Const *) lsecond(f->args))->constvalue) :
		OidFunctionCall2(f->funcid, ((Const *) linitial(f->args))->constvalue, TimestampGetDatum(x));
	int			c;

	if (is_text)
		c = strcmp(TextDatumGetCString(r), TextDatumGetCString(s->operand));
	else if (f->funcresulttype == NUMERICOID)
		c = DatumGetInt32(DirectFunctionCall2(numeric_cmp, r, s->operand));
	else
		c = (DatumGetFloat8(r) > DatumGetFloat8(s->operand)) - (DatumGetFloat8(r) < DatumGetFloat8(s->operand));
	return (c > 0) - (c < 0);
}

/* the window of the step at or before x, in the clock of the column's type (a timestamptz in the session's zone), and the one after the window that starts at ws */
static Timestamp
window_start(Oid ttype, Window win, Timestamp x)
{
	static const char *const unit[] = {NULL, "year", "month", "day", "hour", "minute"};

	return DatumGetTimestamp(DirectFunctionCall2(ttype == TIMESTAMPOID ? timestamp_trunc : timestamptz_trunc, CStringGetTextDatum(unit[win]), TimestampGetDatum(x)));
}

static Timestamp
window_next(Oid ttype, Window win, Timestamp ws)
{
	Interval	iv = {.time = win == WIN_HOUR ? USECS_PER_HOUR : win == WIN_MINUTE ? USECS_PER_MINUTE : 0,.day = win == WIN_DAY,.month = win == WIN_YEAR ? 12 : win == WIN_MONTH};
	const Timestamp step = DatumGetTimestamp(DirectFunctionCall2(ttype == TIMESTAMPOID ? timestamp_pl_interval : timestamptz_pl_interval, TimestampGetDatum(ws), PointerGetDatum(&iv)));
	const Timestamp next = window_start(ttype, win, step);

	/* the truncation of an instant of a repeated hour may give the window it started from: the step itself is then the window */
	return next > ws ? next : step;
}

/* the most windows a span holds, by their shortest length (a day of a clock change is 23 hours) */
static int64
windows_of(Window win, Timestamp min, Timestamp max)
{
	static const int64 shortest[] = {0, 365 * USECS_PER_DAY, 28 * USECS_PER_DAY, 23 * USECS_PER_HOUR, USECS_PER_HOUR / 2, USECS_PER_MINUTE};

	return win == 0 ? 1 : (max - min) / shortest[win] + 1;
}

/* the instants of [min, max] where the clock of the session's zone goes back (the end of daylight time): the fields of the clock are not in order across one; -1 for too many */
static int
clock_cuts(Timestamp min, Timestamp max, Timestamp **cuts)
{
	pg_time_t	t = timestamptz_to_time_t(min),
				end = timestamptz_to_time_t(max),
				boundary;
	long int	before,
				after;
	int			bdst,
				adst,
				n = 0,
				cap = 16;

	*cuts = palloc(cap * sizeof(Timestamp));
	while (t <= end && pg_next_dst_boundary(&t, &before, &bdst, &boundary, &after, &adst, session_timezone) == 1 && boundary <= end)
	{
		if (after < before)
		{
			if (n == PERIODIC_MAX_CUTS)
				return -1;
			if (n == cap)
				*cuts = repalloc(*cuts, (cap *= 2) * sizeof(Timestamp));
			(*cuts)[n++] = time_t_to_timestamptz(boundary);
		}
		t = boundary;
	}
	return n;
}

/* the first x of [a, b] with f(x) >= operand (above: > operand), given that f(a) is below it and f(b) is not */
static Timestamp
bisect(const Step *s, Timestamp a, Timestamp b, bool above)
{
	while (b - a > 1)
	{
		const Timestamp m = a + (b - a) / 2;

		if (step_cmp(s, m) > (above ? 0 : -1))
			b = m;
		else
			a = m;
	}
	return b;
}

static void
add_range(SiftV6RangeSet *out, int slot, bool has_lo, Timestamp lo, bool has_hi, Timestamp hi)
{
	SiftV6Range *r = &out->range[out->nranges++];

	r->nbounds = 1;
	r->bound[0] = (SiftV6Bound) {.slot = slot,.has_lo = has_lo,.lo = lo,.has_hi = has_hi,.hi = hi};
}

/* the interval of the window [ws, we] where f equals the operand, when it has one (inside a window f never goes back), within the span [min, max] */
static void
window_range(const Step *s, int slot, Timestamp ws, Timestamp we, Timestamp min, Timestamp max, SiftV6RangeSet *out, double *covered)
{
	Timestamp	lo = ws,
				hi = we;

	if (step_cmp(s, ws) > 0 || step_cmp(s, we) < 0)
		return;
	if (step_cmp(s, ws) < 0)
		lo = bisect(s, ws, we, false);
	if (step_cmp(s, lo) != 0)
		return;
	if (step_cmp(s, we) > 0)
		hi = bisect(s, lo, we, true) - 1;
	lo = Max(lo, min);
	hi = Min(hi, max);
	if (lo > hi)
		return;
	add_range(out, slot, true, lo, true, hi);
	*covered += (double) hi - (double) lo + 1;
}

/* the windows of [min, max] and the ranges of each, in a context of their own (the calls of f allocate); false when there are too many or they keep more than a share of the span */
static bool
windows_ranges(const Step *s, int slot, Timestamp min, Timestamp max, SiftV6RangeSet *out)
{
	MemoryContext calls = AllocSetContextCreate(CurrentMemoryContext, "sift periodic calendar", ALLOCSET_SMALL_SIZES),
				old = MemoryContextSwitchTo(calls);
	Timestamp  *cuts = NULL,
				ws = s->win == 0 ? min : window_start(s->ttype, s->win, min);
	int			ncut = s->ttype == TIMESTAMPTZOID ? clock_cuts(min, max, &cuts) : 0,
				ci = 0,
				nwin = 0;
	double		covered = 0;
	bool		ok = ncut >= 0;

	while (ok && ws <= max)
	{
		Timestamp	next = s->win == 0 ? max + 1 : window_next(s->ttype, s->win, ws);

		while (ci < ncut && cuts[ci] <= ws)
			ci++;
		if (ci < ncut && cuts[ci] < next)
			next = cuts[ci];
		ok = ++nwin <= PERIODIC_MAX_WINDOWS && out->nranges < PERIODIC_MAX_WINDOWS;
		if (ok)
			window_range(s, slot, ws, next - 1, min, max, out, &covered);
		ws = next;
		MemoryContextReset(calls);
	}
	MemoryContextSwitchTo(old);
	MemoryContextDelete(calls);
	return ok && covered * PERIODIC_SHARE <= (double) max - (double) min + 1;
}

/* the span of the column in the statistics of the index, away from the ends of the timestamps (a window there may leave them) and, for a year in text, within the years 1000 to 9999 */
static bool
column_span(const SiftV6Input *in, int slot, const Step *s, Timestamp *min, Timestamp *max)
{
	const SiftCardStatsView *view = in->is_table ? NULL : sift_v6_rel_card_view(in->indexoid);
	const int	leaf = view != NULL ? sift_v6_rel_card_leaf(in->indexoid, in->col[slot].attno - 1) : -1;
	const SiftColCardStats *st = leaf >= 0 && leaf < (int) view->ncols ? &view->cols[leaf] : NULL;
	struct pg_tm tm;
	fsec_t		fsec;

	if (st == NULL || st->total_rows <= st->null_rows || st->min_i64 > st->max_i64)
		return false;
	*min = st->min_i64;
	*max = st->max_i64;
	if (*min < MIN_TIMESTAMP + 400 * USECS_PER_DAY || *max > END_TIMESTAMP - 400 * USECS_PER_DAY || windows_of(s->win, *min, *max) > PERIODIC_MAX_WINDOWS)
		return false;
	return !s->year4 || (timestamp2tm(*min, NULL, &tm, &fsec, NULL, NULL) == 0 && tm.tm_year >= 1001 && timestamp2tm(*max, NULL, &tm, &fsec, NULL, NULL) == 0 && tm.tm_year <= 9998);
}

bool
sift_v6_periodic_ranges(MemoryContext keep, const Node *clause, Index relid, ParamListInfo params, SiftV6Input *in, SiftV6RangeSet *out)
{
	Step		s;
	const Var  *var;
	Timestamp	min,
				max;
	int			slot;

	if (!equality_of(clause, relid, params, &s, &var) || (slot = sift_v6_input_add_var(in, var)) < 0 || !column_span(in, slot, &s, &min, &max))
		return false;
	*out = (SiftV6RangeSet) {.range = MemoryContextAlloc(keep, (PERIODIC_MAX_WINDOWS + 2) * sizeof(SiftV6Range)),.periodic = true};
	if (!windows_ranges(&s, slot, min, max, out))
		return false;
	/* the statistics may be older than the rows: what lies outside their span is kept */
	add_range(out, slot, false, 0, true, min - 1);
	add_range(out, slot, true, max + 1, false, 0);
	return true;
}
