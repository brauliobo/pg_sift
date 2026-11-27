/*
 * agg/qual/step.c - the calendar step of an expression of a timestamp column, and the runs of rows of one step in a batch (agg/qual/step.h).
 */
#include "postgres.h"

#include "catalog/pg_type_d.h"
#include "datatype/timestamp.h"
#include "nodes/nodeFuncs.h"
#include "utils/builtins.h"
#include "utils/fmgroids.h"
#include "utils/timestamp.h"

#include "agg/qual/step.h"

typedef struct StepUnit
{
	const char *name;
	int64		us;
} StepUnit;

/* the units of date_trunc: a week and the longer ones change on a day */
static const StepUnit trunc_units[] = {{"second", USECS_PER_SEC}, {"minute", USECS_PER_MINUTE}, {"hour", USECS_PER_HOUR}, {"day", USECS_PER_DAY}, {"week", USECS_PER_DAY},
	{"month", USECS_PER_DAY}, {"quarter", USECS_PER_DAY}, {"year", USECS_PER_DAY}, {"decade", USECS_PER_DAY}, {"century", USECS_PER_DAY}, {"millennium", USECS_PER_DAY}, {NULL, 0}};

/* the fields of extract that are whole numbers of a step (second, epoch and julian have fractions of a second) */
static const StepUnit field_units[] = {{"minute", USECS_PER_MINUTE}, {"hour", USECS_PER_HOUR}, {"day", USECS_PER_DAY}, {"dow", USECS_PER_DAY}, {"isodow", USECS_PER_DAY},
	{"doy", USECS_PER_DAY}, {"week", USECS_PER_DAY}, {"month", USECS_PER_DAY}, {"quarter", USECS_PER_DAY}, {"year", USECS_PER_DAY}, {"isoyear", USECS_PER_DAY},
	{"decade", USECS_PER_DAY}, {"century", USECS_PER_DAY}, {"millennium", USECS_PER_DAY}, {NULL, 0}};

typedef enum Clock
{
	CLOCK_ANY,					/* none seen yet, or a timestamp: its values are its clock */
	CLOCK_SESSION,				/* a calendar call of a timestamptz cuts the session's clock */
	CLOCK_INSTANT				/* date_bin of a timestamptz cuts the instants */
} Clock;

typedef struct StepCtx
{
	Index		relid;
	const Var  *col;
	Clock		clock;
	int64		step;			/* the finest step of the calls seen, INT64_MAX before the first */
	bool		ok;
} StepCtx;

/* the step of the unit named by the first argument of a call (a text Const), 0 for one the table does not have */
static int64
unit_step(const FuncExpr *f, const StepUnit *units)
{
	const Const *k = linitial(f->args);

	if (!IsA(k, Const) || k->constisnull || k->consttype != TEXTOID)
		return 0;
	for (const StepUnit *u = units; u->name != NULL; u++)
		if (pg_strcasecmp(u->name, TextDatumGetCString(k->constvalue)) == 0)
			return u->us;
	return 0;
}

/* date_bin(stride, x, origin) of whole seconds changes only where x crosses origin + k * stride: on the multiples of gcd(stride, origin), counted from 2000-01-01. 0 for a stride with months, or an origin or a stride that is no constant of whole seconds */
static int64
bin_step(const FuncExpr *f)
{
	const Const *s = linitial(f->args),
			   *o = lthird(f->args);
	const Interval *iv;
	int64		us,
				org;

	if (!IsA(s, Const) || !IsA(o, Const) || s->constisnull || o->constisnull)
		return 0;
	iv = DatumGetIntervalP(s->constvalue);
	us = (int64) iv->day * USECS_PER_DAY + iv->time;
	org = DatumGetTimestamp(o->constvalue);
	if (iv->month != 0 || us <= 0 || us % USECS_PER_SEC != 0 || TIMESTAMP_NOT_FINITE(org))
		return 0;
	org = ((org % us) + us) % us;
	if (org % USECS_PER_SEC != 0)
		return 0;
	while (org != 0)
	{
		const int64 t = us % org;

		us = org;
		org = t;
	}
	return us;
}

/* a constant full name of a zone (an area and a city: no abbreviation of the session can be named so) whose offset is 0 at every instant */
static bool
utc_zone(const Node *e)
{
	const Const *k = (const Const *) e;
	char	   *name;

	if (!IsA(e, Const) || k->constisnull || k->consttype != TEXTOID)
		return false;
	name = TextDatumGetCString(k->constvalue);
	return strchr(name, '/') != NULL && pg_tzset(name) != NULL && zone_is_utc(pg_tzset(name));
}

/* the step of a calendar call, the argument it reads in *arg, whether it is a date_bin and whether it cuts a timestamptz on its instants (a date_bin, a date_trunc in a zone of UTC); 0 for any other node */
static int64
call_step(const Node *n, const Node **arg, bool *bin, bool *instant)
{
	const FuncExpr *f = (const FuncExpr *) n;

	if (!IsA(n, FuncExpr) || f->args == NIL)
		return 0;
	*arg = llast(f->args);
	*bin = false;
	*instant = false;
	switch (f->funcid)
	{
		case F_DATE_TRUNC_TEXT_TIMESTAMP:
		case F_DATE_TRUNC_TEXT_TIMESTAMPTZ:
			return unit_step(f, trunc_units);
		case F_DATE_TRUNC_TEXT_TIMESTAMPTZ_TEXT:
			*arg = lsecond(f->args);
			*instant = true;
			return utc_zone(lthird(f->args)) ? unit_step(f, trunc_units) : 0;
		case F_EXTRACT_TEXT_TIMESTAMP:
		case F_EXTRACT_TEXT_TIMESTAMPTZ:
		case F_DATE_PART_TEXT_TIMESTAMP:
		case F_DATE_PART_TEXT_TIMESTAMPTZ:
			return unit_step(f, field_units);
		case F_DATE_TIMESTAMP:
		case F_DATE_TIMESTAMPTZ:
			return USECS_PER_DAY;
		case F_DATE_BIN_INTERVAL_TIMESTAMP_TIMESTAMP:
		case F_DATE_BIN_INTERVAL_TIMESTAMPTZ_TIMESTAMPTZ:
			*arg = lsecond(f->args);
			*bin = true;
			*instant = true;
			return bin_step(f);
		default:
			return 0;
	}
}

/* the column, through relabelings, or a timestamptz column AT TIME ZONE a zone of UTC (its instants as a timestamp: *instant) */
static const Var *
column_of(const Node *n, Index relid, bool *instant)
{
	const FuncExpr *f = (const FuncExpr *) n;

	*instant = IsA(n, FuncExpr) && f->funcid == F_TIMEZONE_TEXT_TIMESTAMPTZ && list_length(f->args) == 2 && utc_zone(linitial(f->args));
	if (*instant)
		n = lsecond(f->args);
	while (IsA(n, RelabelType))
		n = (const Node *) ((const RelabelType *) n)->arg;
	return IsA(n, Var) && ((const Var *) n)->varno == relid && ((const Var *) n)->varlevelsup == 0 &&
		(((const Var *) n)->vartype == TIMESTAMPOID || ((const Var *) n)->vartype == TIMESTAMPTZOID) ? (const Var *) n : NULL;
}

/* the column is the one of the other calls, and the call counts its steps on their clock */
static bool
same_column(StepCtx *c, const Var *v, bool instant)
{
	const Clock clock = v->vartype != TIMESTAMPTZOID ? CLOCK_ANY : instant ? CLOCK_INSTANT : CLOCK_SESSION;

	if ((c->col != NULL && (c->col->varattno != v->varattno || c->col->vartype != v->vartype)) || (c->clock != CLOCK_ANY && c->clock != clock))
		return false;
	c->col = v;
	c->clock = clock;
	return true;
}

/*
 * The step of a calendar call: over the column its own; over another call the coarser of the two, as a truncation or a field keeps the windows of every coarser unit, but the inner step
 * when either is a date_bin (a bin of an odd origin crosses the windows of the other); 0 when its argument is neither
 */
static int64
chain_step(StepCtx *c, const Node *n)
{
	const Node *arg;
	bool		bin,
				instant,
				conv,
				inner_bin;
	const int64 own = call_step(n, &arg, &bin, &instant);
	const Var  *v;
	int64		inner;

	if (own == 0)
		return 0;
	if ((v = column_of(arg, c->relid, &conv)) != NULL)
		return same_column(c, v, instant || conv) ? own : 0;
	inner = chain_step(c, arg);
	if (inner == 0)
		return 0;
	call_step(arg, &arg, &inner_bin, &conv);
	return bin || inner_bin ? inner : Max(inner, own);
}

static bool
step_walker(Node *n, StepCtx *c)
{
	int64		s;

	if (n == NULL || !c->ok)
		return false;
	if ((s = chain_step(c, n)) > 0)
	{
		c->step = Min(c->step, s);
		return false;
	}
	if (IsA(n, Var))
	{
		c->ok = false;			/* a column read as it is, or through a call that is no calendar step */
		return true;
	}
	return expression_tree_walker(n, step_walker, c);
}

int64
sift_v6_calendar_step(const Node *e, Index relid, const Var **col, bool *local)
{
	StepCtx		c = {.relid = relid,.step = INT64_MAX,.ok = true};

	step_walker((Node *) e, &c);
	if (!c.ok || c.col == NULL)
		return 0;
	*col = c.col;
	*local = c.clock == CLOCK_SESSION;
	return c.step;
}

bool
sift_v6_step_open(MemoryContext cx, const Node *e, Index relid, SiftV6Step *out, const Var **col)
{
	bool		local;

	out->us = sift_v6_calendar_step(e, relid, col, &local);
	out->zone = out->us > 0 && local ? zone_open(cx, session_timezone) : NULL;
	return out->us > 0;
}

/* floor(t / us) */
static int64
step_of(int64 t, int64 us)
{
	return t >= 0 ? t / us : -((-(t + 1)) / us) - 1;
}

static bool
finite_at(const SiftVec *v, uint16 r)
{
	return (v->all_valid || ((v->valid[r >> 6] >> (r & 63)) & 1)) && !TIMESTAMP_NOT_FINITE(((const int64 *) v->data)[r]);
}

/* the zone covers the finite instants of the active rows */
static void
cover(const SiftV6Step *st, const SiftVec *v, const SiftBatchSel *bs)
{
	const int64 *t = v->data;
	int64		lo = PG_INT64_MAX,
				hi = PG_INT64_MIN;

	for (uint16 w = 0; w < (bs->n_raw + 63) / 64; w++)
		for (uint64 bits = bs->mask[w]; bits != 0; bits &= bits - 1)
		{
			const uint16 r = (uint16) (w * 64 + __builtin_ctzll(bits));

			if (finite_at(v, r))
			{
				lo = Min(lo, t[r]);
				hi = Max(hi, t[r]);
			}
		}
	if (lo <= hi)
		zone_cover(st->zone, lo, hi);
}

void
sift_v6_step_runs(const SiftV6Step *st, const SiftVec *v, const SiftBatchSel *bs, SiftBatchSel *heads, uint16 *head_of)
{
	const int64 *t = v->data;
	int64		k = 0,
				off = 0;
	bool		open = false;
	uint16		head = 0;

	*heads = (SiftBatchSel) {.n_raw = bs->n_raw};
	if (st->zone != NULL)
		cover(st, v, bs);
	for (uint16 w = 0; w < (bs->n_raw + 63) / 64; w++)
		for (uint64 bits = bs->mask[w]; bits != 0; bits &= bits - 1)
		{
			const uint16 r = (uint16) (w * 64 + __builtin_ctzll(bits));
			const bool	finite = finite_at(v, r);
			const int64 o = finite && st->zone != NULL ? zone_offset_at(st->zone, t[r]) : 0;
			const int64 s = finite ? step_of(t[r] + o, st->us) : 0;

			if (!(open && finite && s == k && o == off))
			{
				head = r;
				heads->mask[w] |= UINT64CONST(1) << (r & 63);
				heads->n_active++;
			}
			head_of[r] = head;
			open = finite;
			k = s;
			off = o;
		}
	heads->all_active = heads->n_active == heads->n_raw;
}

const char *
sift_v6_step_text(int64 us)
{
	switch (us)
	{
		case USECS_PER_SEC:
			return " (per second)";
		case USECS_PER_MINUTE:
			return " (per minute)";
		case USECS_PER_HOUR:
			return " (per hour)";
		case USECS_PER_DAY:
			return " (per day)";
		default:
			return psprintf(" (per " INT64_FORMAT " seconds)", us / USECS_PER_SEC);
	}
}
