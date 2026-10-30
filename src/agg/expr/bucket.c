/*
 * agg/expr/bucket.c - the steps and buckets of a key
 * The seconds of a truncation or of a field, the groups of a stepped key, and the
 * bounds a predicate on it gives.
 */
#include "postgres.h"

#include "nodes/nodeFuncs.h"
#include "optimizer/optimizer.h"
#include "utils/fmgroids.h"

#include "agg/expr/int.h"

/*
 * Calendar keys of a timestamp leaf. date_trunc, ::date and extract of a timestamp change value only on a second, minute, hour or
 * day boundary, so the executor evaluates them once per such step of the data instead of once per distinct timestamp (which is
 * once per row): the key id of a row is a lookup of its step.
 */

/* date_trunc units (a week, month and longer ones change on a day) */
const BucketUnit trunc_units[] = {{"second", 1, 1, 0}, {"minute", 60, 60, 0}, {"hour", 3600, 3600, 0}, {"day", DAY_SECS, DAY_SECS, 0},
	{"week", DAY_SECS, 7 * DAY_SECS, 0}, {"month", DAY_SECS, MONTH_SECS, 0}, {"quarter", DAY_SECS, 3 * MONTH_SECS, 0},
	{"year", DAY_SECS, YEAR_SECS, 0}, {"decade", DAY_SECS, 10 * YEAR_SECS, 0}, {"century", DAY_SECS, 100 * YEAR_SECS, 0},
	{"millennium", DAY_SECS, 1000 * YEAR_SECS, 0}, {NULL, 0, 0, 0}};

/* extract fields that are whole numbers of a step (second, epoch and julian have fractions of a second) */
const BucketUnit field_units[] = {{"minute", 60, 60, 60}, {"hour", 3600, 3600, 24}, {"day", DAY_SECS, DAY_SECS, 31},
	{"dow", DAY_SECS, DAY_SECS, 7}, {"isodow", DAY_SECS, DAY_SECS, 7}, {"doy", DAY_SECS, DAY_SECS, 366}, {"week", DAY_SECS, 7 * DAY_SECS, 53},
	{"month", DAY_SECS, MONTH_SECS, 12}, {"quarter", DAY_SECS, 3 * MONTH_SECS, 4}, {"year", DAY_SECS, YEAR_SECS, 0},
	{"isoyear", DAY_SECS, YEAR_SECS, 0}, {"decade", DAY_SECS, 10 * YEAR_SECS, 0}, {"century", DAY_SECS, 100 * YEAR_SECS, 0},
	{"millennium", DAY_SECS, 1000 * YEAR_SECS, 0}, {NULL, 0, 0, 0}};

typedef struct StepCtx
{
	AggCtx	   *c;
	int			leaf;
	int			secs;
	int			zoned;
	bool		ok;
} StepCtx;

/* the unit of a date_trunc, date_part or extract of a timestamp (of a timestamptz in the session's zone with `session`); NULL for another call, or a unit this does not know */
const BucketUnit *
bucket_unit(Node *e, bool session)
{
	FuncExpr   *f;
	Const	   *k;
	char	   *name;
	bool		trunc;

	if (!IsA(e, FuncExpr))
		return NULL;
	f = (FuncExpr *) e;
	if (session)
	{
		if (!session_zone_call(f->funcid) || f->funcid == F_DATE_TIMESTAMPTZ)
			return NULL;
		trunc = f->funcid == F_DATE_TRUNC_TEXT_TIMESTAMPTZ;
	}
	else
	{
		/* date_trunc(unit, timestamptz, 'UTC') cuts the UTC clock, the same in every session */
		trunc = f->funcid == F_DATE_TRUNC_TEXT_TIMESTAMP || (f->funcid == F_DATE_TRUNC_TEXT_TIMESTAMPTZ_TEXT && utc_name(lthird(f->args)));
		if (!trunc && f->funcid != F_DATE_PART_TEXT_TIMESTAMP && f->funcid != F_EXTRACT_TEXT_TIMESTAMP)
			return NULL;
	}
	k = linitial(f->args);
	if (!IsA(k, Const) || k->constisnull || k->consttype != TEXTOID)
		return NULL;
	name = TextDatumGetCString(k->constvalue);
	for (const BucketUnit *u = trunc ? trunc_units : field_units; u->name; u++)
		if (pg_strcasecmp(u->name, name) == 0)
			return u;
	return NULL;
}

/* the leaf of the expression of a key: a reference, or Param 0 once the key is compiled */
static bool
bucket_leaf(AggCtx *c, Node *e)
{
	return IsA(e, Param) || match_leaf(c, e) >= 0;
}

/*
 * The seconds an expression of the timestamp leaf keeps its value for: the leaf itself 1 (every function below has a step of a
 * second or more), a calendar function the coarser of its own step and the one of its argument. 0: the expression is something
 * else, and the key is a combo.
 */
static int
bucket_step(AggCtx *c, Node *e, bool session)
{
	const BucketUnit *u;
	int			arg;

	/* in the session's zone the leaf is read only by the calendar call right above it: a function of the UTC clock below it would cut the day on another clock */
	if (bucket_leaf(c, e))
		return session ? 0 : 1;
	if (IsA(e, RelabelType))
		return bucket_step(c, (Node *) ((RelabelType *) e)->arg, session);
	if (!IsA(e, FuncExpr))
		return 0;
	if (session && session_zone_call(((FuncExpr *) e)->funcid))
	{
		FuncExpr   *f = (FuncExpr *) e;
		Node	   *x = (Node *) llast(f->args);
		int			own;

		if (list_length(f->args) > 2)
			return 0;
		if (f->funcid == F_DATE_TIMESTAMPTZ)
			own = DAY_SECS;
		else if ((u = bucket_unit(e, true)) != NULL)
			own = u->secs;
		else
			return 0;
		/* over the call of a coarser step (date(date_trunc('day', ts))) the value is a function of that step */
		if (bucket_leaf(c, x))
			return own;
		arg = bucket_step(c, x, true);
		return arg ? Max(arg, own) : 0;
	}
	if (is_utc_conversion(e))
		return bucket_step(c, lsecond(((FuncExpr *) e)->args), session);
	if (is_num_cast(e))
		return bucket_step(c, linitial(((FuncExpr *) e)->args), session);
	if ((u = bucket_unit(e, false)) != NULL)
	{
		arg = bucket_step(c, lsecond(((FuncExpr *) e)->args), session);
		return arg ? Max(arg, u->secs) : 0;
	}
	switch (((FuncExpr *) e)->funcid)
	{
		case F_DATE_TIMESTAMP:
			arg = bucket_step(c, linitial(((FuncExpr *) e)->args), session);
			return arg ? Max(arg, DAY_SECS) : 0;
		case F_TIMESTAMP_DATE:
			return bucket_step(c, linitial(((FuncExpr *) e)->args), session);
		default:
			return 0;
	}
}

/* the distinct values an expression that bucket_step accepts takes over a stretch of `span` seconds of the leaf */
double
bucket_groups(AggCtx *c, Node *e, double span)
{
	const BucketUnit *u;
	double		n;

	if (bucket_leaf(c, e))
		return span + 1;
	if (IsA(e, RelabelType))
		return bucket_groups(c, (Node *) ((RelabelType *) e)->arg, span);
	if (is_utc_conversion(e))
		return bucket_groups(c, lsecond(((FuncExpr *) e)->args), span);
	if (is_num_cast(e))
		return bucket_groups(c, linitial(((FuncExpr *) e)->args), span);
	if ((u = bucket_unit(e, false)) != NULL || (u = bucket_unit(e, true)) != NULL)
	{
		n = Min(bucket_groups(c, lsecond(((FuncExpr *) e)->args), span), span / u->len + 1);
		return u->cycle ? Min(n, u->cycle) : n;
	}
	if (((FuncExpr *) e)->funcid == F_DATE_TIMESTAMP || ((FuncExpr *) e)->funcid == F_DATE_TIMESTAMPTZ)
		return Min(bucket_groups(c, linitial(((FuncExpr *) e)->args), span), span / DAY_SECS + 1);
	return bucket_groups(c, linitial(((FuncExpr *) e)->args), span);
}

/* EXPLAIN: the step a key on a timestamp is evaluated for */
const char *
bucket_text(int secs)
{
	return secs == 1 ? " (per second)" : secs == 60 ? " (per minute)" : secs == 3600 ? " (per hour)" : " (per day)";
}

/*
 * date_trunc of a timestamptz to a second, minute or hour keeps the offset of the instant it cuts (the other units read the offset of the zone at the start of the day): the two
 * instants of a repeated hour are two values. Is there such a call in the chain of calls down to the leaf: 0 no, 1 it is the top call, -1 it has another call above it.
 */
static int
cut_trunc(Node *e)
{
	bool		top = true;

	for (;;)
	{
		FuncExpr   *f;
		const BucketUnit *u;

		if (IsA(e, RelabelType))
		{
			e = (Node *) ((RelabelType *) e)->arg;
			continue;
		}
		if (!IsA(e, FuncExpr))
			return 0;
		f = (FuncExpr *) e;
		if (f->funcid == F_DATE_TRUNC_TEXT_TIMESTAMPTZ && (u = bucket_unit(e, true)) != NULL && u->secs <= 3600)
			return top ? 1 : -1;
		top = false;
		e = (Node *) llast(f->args);
	}
}

/*
 * The seconds a key on the timestamp leaf is evaluated once for, 0 when it is not a calendar expression. A timestamptz key is the same in every session only if it reads
 * the UTC clock (AT TIME ZONE 'UTC'); one that reads the session's zone (date_trunc('day', ts), extract(hour FROM ts), ts::date) is cut on the local clock of the zone the
 * executor finds when it starts (*zoned).
 */
int
bucket_key_secs(AggCtx *c, int leaf, Node *expr, int *zoned)
{
	SiftSrcLeaf *l = leaf_of(c, leaf);
	uint8		mask;

	*zoned = 0;
	if (l->kind != SCL_SCALAR || l->vtype != SCV_I64 || (l->typid != TIMESTAMPOID && l->typid != TIMESTAMPTZOID) || contain_volatile_functions(expr))
		return 0;
	if (l->typid == TIMESTAMPTZOID && sift_aggexpr_cal(expr, &mask) != SAC_OK)
	{
		int			secs = bucket_step(c, expr, true),
					cut = cut_trunc(expr);

		/* a cut to the second, minute or hour under another call is not a function of the step */
		if (secs <= 0 || cut < 0)
			return 0;
		*zoned = cut > 0 ? SIFT_ZONE_CUT : SIFT_ZONE_STEP;
		return secs;
	}
	return bucket_step(c, expr, false);
}

/* every read of the leaf is inside a calendar expression of one clock (bucket_key_secs), and the coarsest of their steps is the one the condition changes on */
static bool
step_walker(Node *n, StepCtx *x)
{
	int			z,
				secs;

	if (n == NULL || !x->ok)
		return false;
	if ((IsA(n, FuncExpr) || IsA(n, RelabelType)) && (secs = bucket_key_secs(x->c, x->leaf, n, &z)) > 0)
	{
		if (z == SIFT_ZONE_CUT || (x->secs && x->zoned != z))
			x->ok = false;
		else
		{
			x->secs = Max(x->secs, secs);
			x->zoned = z;
		}
		return false;
	}
	if (match_leaf(x->c, n) >= 0)
	{
		x->ok = false;
		return false;
	}
	return expression_tree_walker(n, step_walker, x);
}

/* the seconds a boolean expression of the timestamp leaf is evaluated once for, 0 when it reads the leaf other than through calendar calls (a key's rule, for each call of the condition) */
int
bucket_pred_secs(AggCtx *c, int leaf, Node *expr, int *zoned)
{
	StepCtx		x = {c, leaf, 0, 0, true};

	*zoned = 0;
	if (contain_volatile_functions(expr) || raising_walker(expr, NULL))
		return 0;
	(void) step_walker(expr, &x);
	*zoned = x.zoned;
	return x.ok ? x.secs : 0;
}
