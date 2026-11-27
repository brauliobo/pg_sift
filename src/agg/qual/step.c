/*
 * agg/qual/step.c - the calendar step of an expression of a timestamp column (agg/qual/step.h).
 */
#include "postgres.h"

#include "catalog/pg_type_d.h"
#include "datatype/timestamp.h"
#include "nodes/nodeFuncs.h"
#include "utils/builtins.h"
#include "utils/fmgroids.h"

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

typedef struct StepCtx
{
	Index		relid;
	const Var  *col;
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

/* the step of a calendar call of the column's clock, and the argument it reads in *arg; 0 for any other node */
static int64
call_step(const Node *n, const Node **arg)
{
	const FuncExpr *f = (const FuncExpr *) n;

	if (!IsA(n, FuncExpr) || f->args == NIL)
		return 0;
	*arg = llast(f->args);
	switch (f->funcid)
	{
		case F_DATE_TRUNC_TEXT_TIMESTAMP:
		case F_DATE_TRUNC_TEXT_TIMESTAMPTZ:
			return unit_step(f, trunc_units);
		case F_EXTRACT_TEXT_TIMESTAMP:
		case F_EXTRACT_TEXT_TIMESTAMPTZ:
		case F_DATE_PART_TEXT_TIMESTAMP:
		case F_DATE_PART_TEXT_TIMESTAMPTZ:
			return unit_step(f, field_units);
		case F_DATE_TIMESTAMP:
		case F_DATE_TIMESTAMPTZ:
			return USECS_PER_DAY;
		default:
			return 0;
	}
}

/* the column, through relabelings */
static const Var *
column_of(const Node *n, Index relid)
{
	while (IsA(n, RelabelType))
		n = (const Node *) ((const RelabelType *) n)->arg;
	return IsA(n, Var) && ((const Var *) n)->varno == relid && ((const Var *) n)->varlevelsup == 0 &&
		(((const Var *) n)->vartype == TIMESTAMPOID || ((const Var *) n)->vartype == TIMESTAMPTZOID) ? (const Var *) n : NULL;
}

/* the step of a calendar call: over the column its own, over another call the coarser of the two (the value is a function of that step); 0 when its argument is neither */
static int64
chain_step(StepCtx *c, const Node *n)
{
	const Node *arg;
	const int64 own = call_step(n, &arg);
	const Var  *v;
	int64		inner;

	if (own == 0)
		return 0;
	if ((v = column_of(arg, c->relid)) != NULL)
	{
		if (c->col != NULL && (c->col->varattno != v->varattno || c->col->vartype != v->vartype))
			return 0;
		c->col = v;
		return own;
	}
	inner = chain_step(c, arg);
	return inner == 0 ? 0 : Max(inner, own);
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
sift_v6_calendar_step(const Node *e, Index relid, const Var **col)
{
	StepCtx		c = {.relid = relid,.step = INT64_MAX,.ok = true};

	step_walker((Node *) e, &c);
	if (!c.ok || c.col == NULL)
		return 0;
	*col = c.col;
	return c.step;
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
