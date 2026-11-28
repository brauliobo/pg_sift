/*
 * agg/qual/grain.c - the calendar cells an expression of a date or timestamp column keeps its value over (agg/qual/grain.h).
 *
 * A chain of calendar calls over the column is followed from the column out. Each call keeps its value over the cells its unit spans (keep) and leaves every instant in the cells of the
 * grains it does not cross (stays: date_trunc('hour') leaves an instant in its day, month and year, date_trunc('week') may move it to the month before), so a call over a call keeps its
 * value over the cells of the inner one and over its own cells the inner one stays in. A call reads its argument on one clock: the calendar of a date or a timestamp (plain), of a timestamptz
 * in the session's zone (local: the midnight of a date cast to timestamptz) or of its instants (UTC: the column itself, whose cells are UTC days); a chain that reads another clock is no chain.
 */
#include "postgres.h"

#include "catalog/pg_type_d.h"
#include "nodes/nodeFuncs.h"
#include "parser/scansup.h"
#include "utils/builtins.h"
#include "utils/fmgroids.h"

#include "agg/qual/grain.h"
#include "agg/qual/step.h"

#define DAY		SIFT_V6_CELL_DAY
#define MONTH	SIFT_V6_CELL_MONTH
#define YEAR	SIFT_V6_CELL_YEAR
#define ALL		(DAY | MONTH | YEAR)

typedef struct GrainUnit
{
	const char *name;
	int			keep;			/* the cells its value is constant over */
	int			stays;			/* a truncation: the cells it leaves every instant in */
} GrainUnit;

static const GrainUnit trunc_units[] = {{"microseconds", 0, ALL}, {"milliseconds", 0, ALL}, {"second", 0, ALL}, {"minute", 0, ALL}, {"hour", 0, ALL}, {"day", DAY, ALL},
	{"week", DAY, 0}, {"month", DAY | MONTH, MONTH | YEAR}, {"quarter", DAY | MONTH, YEAR}, {"year", ALL, YEAR}, {"decade", ALL, 0}, {"century", ALL, 0}, {"millennium", ALL, 0}, {NULL}};

static const GrainUnit field_units[] = {{"day", DAY}, {"dow", DAY}, {"isodow", DAY}, {"doy", DAY}, {"week", DAY}, {"isoyear", DAY}, {"month", DAY | MONTH}, {"quarter", DAY | MONTH},
	{"year", ALL}, {"decade", ALL}, {"century", ALL}, {"millennium", ALL}, {NULL}};

/* a cast between a date and a timestamp, and the date of a timestamp */
static const GrainUnit cast_unit = {NULL, 0, ALL};
static const GrainUnit date_unit = {NULL, DAY, ALL};

typedef enum Clock
{
	CK_NUMBER,					/* a field: no calendar */
	CK_PLAIN,
	CK_LOCAL,
	CK_UTC
} Clock;

typedef struct Chain
{
	Clock		clock;
	int			keep;
	int			stays;
} Chain;

static bool chain_of(const Node *n, Index relid, Chain *out);

/* the unit named by the first argument of a call (a text Const), NULL for one the table does not have */
static const GrainUnit *
unit_of(const FuncExpr *f, const GrainUnit *units)
{
	const Const *k = linitial(f->args);
	char	   *name;

	if (!IsA(k, Const) || k->constisnull || k->consttype != TEXTOID)
		return NULL;
	name = TextDatumGetCString(k->constvalue);
	name = downcase_truncate_identifier(name, strlen(name), false);
	for (const GrainUnit *u = units; u->name != NULL; u++)
		if (strcmp(u->name, name) == 0)
			return u;
	return NULL;
}

/* A calendar call: the clock it reads its argument on (*in), the clock of its result, what it keeps and where it leaves an instant; false for any other call */
static bool
call_unit(const FuncExpr *f, const Node **arg, Clock *in, Chain *own)
{
	const GrainUnit *u = &cast_unit;

	*arg = llast(f->args);
	switch (f->funcid)
	{
		case F_DATE_TRUNC_TEXT_TIMESTAMP:
		case F_DATE_TRUNC_TEXT_TIMESTAMPTZ:
			*in = own->clock = f->funcid == F_DATE_TRUNC_TEXT_TIMESTAMP ? CK_PLAIN : CK_LOCAL;
			u = unit_of(f, trunc_units);
			break;
		case F_DATE_TRUNC_TEXT_TIMESTAMPTZ_TEXT:
			*arg = lsecond(f->args);
			*in = own->clock = CK_UTC;
			u = sift_v6_utc_zone(lthird(f->args)) ? unit_of(f, trunc_units) : NULL;
			break;
		case F_EXTRACT_TEXT_DATE:
		case F_EXTRACT_TEXT_TIMESTAMP:
		case F_DATE_PART_TEXT_DATE:
		case F_DATE_PART_TEXT_TIMESTAMP:
		case F_EXTRACT_TEXT_TIMESTAMPTZ:
		case F_DATE_PART_TEXT_TIMESTAMPTZ:
			*in = f->funcid == F_EXTRACT_TEXT_TIMESTAMPTZ || f->funcid == F_DATE_PART_TEXT_TIMESTAMPTZ ? CK_LOCAL : CK_PLAIN;
			own->clock = CK_NUMBER;
			u = unit_of(f, field_units);
			break;
		case F_TIMEZONE_TEXT_TIMESTAMPTZ:
			*in = CK_UTC;
			own->clock = CK_PLAIN;
			u = list_length(f->args) == 2 && sift_v6_utc_zone(linitial(f->args)) ? u : NULL;
			break;
		case F_TIMESTAMP_DATE:
		case F_TIMESTAMPTZ_DATE:
			*in = CK_PLAIN;
			own->clock = f->funcid == F_TIMESTAMP_DATE ? CK_PLAIN : CK_LOCAL;
			break;
		case F_DATE_TIMESTAMP:
		case F_DATE_TIMESTAMPTZ:
			*in = f->funcid == F_DATE_TIMESTAMP ? CK_PLAIN : CK_LOCAL;
			own->clock = CK_PLAIN;
			u = &date_unit;
			break;
		default:
			return false;
	}
	if (u == NULL)
		return false;
	own->keep = u->keep;
	own->stays = own->clock == CK_NUMBER ? 0 : u->stays;
	return true;
}

static bool
call_of(const FuncExpr *f, Index relid, Chain *out)
{
	const Node *arg;
	Clock		in;
	Chain		own,
				a;

	if (f->args == NIL || !call_unit(f, &arg, &in, &own) || !chain_of(arg, relid, &a) || a.clock != in)
		return false;
	*out = (Chain) {.clock = own.clock,.keep = a.keep | (own.keep & a.stays),.stays = own.stays & a.stays};
	return true;
}

/* n is the column, or a chain of calendar calls over it */
static bool
chain_of(const Node *n, Index relid, Chain *out)
{
	const Var  *v;

	while (IsA(n, RelabelType))
		n = (const Node *) ((const RelabelType *) n)->arg;
	v = (const Var *) n;
	if (IsA(n, FuncExpr))
		return call_of((const FuncExpr *) n, relid, out);
	if (!IsA(n, Var) || v->varno != relid || v->varlevelsup != 0 || (v->vartype != DATEOID && v->vartype != TIMESTAMPOID && v->vartype != TIMESTAMPTZOID))
		return false;
	*out = (Chain) {.clock = v->vartype == TIMESTAMPTZOID ? CK_UTC : CK_PLAIN,.keep = 0,.stays = ALL};
	return true;
}

typedef struct GrainCtx
{
	Index		relid;
	int			keep;
	bool		any;
} GrainCtx;

static bool
grain_walker(Node *n, GrainCtx *c)
{
	Chain		ch;

	if (n == NULL)
		return false;
	if (chain_of(n, c->relid, &ch))
	{
		c->keep &= ch.keep;
		c->any = true;
		return c->keep == 0;
	}
	if (IsA(n, Var))
	{
		c->keep = 0;			/* a column read outside a chain */
		return true;
	}
	return expression_tree_walker(n, grain_walker, c);
}

int
sift_v6_calendar_grains(const Node *e, Index relid)
{
	GrainCtx	c = {.relid = relid,.keep = ALL};

	grain_walker((Node *) e, &c);
	return c.any ? c.keep : 0;
}
