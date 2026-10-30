/*
 * agg/expr/cal.c - the calendar units of a key
 * UTC conversions, the parts of a date a key takes, and the groups of a calendar key
 * (day, month, year).
 */
#include "postgres.h"

#include "catalog/pg_namespace_d.h"
#include "nodes/nodeFuncs.h"
#include "optimizer/optimizer.h"
#include "utils/fmgroids.h"
#include "utils/lsyscache.h"

#include "col/preagg.h"
#include "agg/expr/int.h"

/*
 * Calendar expressions of a date, timestamp or timestamptz leaf (Param 0), and the cells of a unit's calendar dim (days, months or years; docs/COL-PREAGG.md
 * section 2) that answer them. The value of an expression is the same for all the days of a cell when it is a function of the cell, which is known from
 * the functions it applies. For each part of it, in the grains 1 (day), 2 (month) and 3 (year):
 *   mask  the grains whose cells it is constant over: a function that depends only on the day (month, year) of its argument is constant over a cell of
 *         that grain or a finer one, and over a coarser one only if its argument stays inside the cell (date_trunc('month', x) is in the month of x, a
 *         week start is not)
 *   stay  the finest grain whose cell of x holds the value, when the part is a timestamp or date (CAL_NONE: none; 'second' to 'day' and the casts stay in the day)
 * The raw timestamp has no grain, a date has the day. A part that uses the session time zone (a timestamptz function, a cast to or from one) is exact at the
 * day of a date and nowhere else: the cells of a month give a different date to a skipped day of the zone, and the instants of a timestamp are not days.
 */
#define CAL_BIT(g)		(1 << (g))
#define CAL_ALL			(CAL_BIT(SIFT_CAL_DAY) | CAL_BIT(SIFT_CAL_MONTH) | CAL_BIT(SIFT_CAL_YEAR))
#define CAL_NONE		(SIFT_CAL_YEAR + 1)

typedef struct CalPart
{
	uint8		mask;
	int			stay;
	bool		zoned;
	bool		date;			/* a timestamp or date, which a function of the calendar may take, not a number */
} CalPart;

typedef struct CalUnits
{
	const BucketUnit *finest;
	int			n;
} CalUnits;

/* a time zone name of UTC (the three-argument date_trunc and AT TIME ZONE with it give the same value in every session, but for a session zone that defines the bare name) */
static bool
utc_spelling(Node *e, char **name)
{
	static const char *const names[] = {"UTC", "Etc/UTC", "GMT", "Etc/GMT", "UCT", "Etc/UCT", "Zulu", "Etc/Zulu"};
	Const	   *k;
	char	   *z;
	bool		ok = false;

	if (!IsA(e, Const) || (k = (Const *) e)->constisnull || k->consttype != TEXTOID)
		return false;
	z = TextDatumGetCString(k->constvalue);
	for (int i = 0; i < lengthof(names); i++)
		ok |= pg_strcasecmp(z, names[i]) == 0;
	if (name != NULL && ok)
		*name = z;
	else
		pfree(z);
	return ok;
}

bool
utc_name(Node *e)
{
	char	   *z;
	bool		ok = utc_spelling(e, &z);

	if (ok)
	{
		ok = strchr(z, '/') != NULL || !sift_session_abbrev_not_utc(z);
		pfree(z);
	}
	return ok;
}

static bool
utc_shadowed_walker(Node *e, void *arg)
{
	char	   *z;

	if (e == NULL)
		return false;
	if (IsA(e, FuncExpr))
	{
		FuncExpr   *f = (FuncExpr *) e;
		Node	   *n = f->funcid == F_DATE_TRUNC_TEXT_TIMESTAMPTZ_TEXT ? lthird(f->args) : f->funcid == F_TIMEZONE_TEXT_TIMESTAMPTZ ? linitial(f->args) : NULL;

		if (n != NULL && utc_spelling(n, &z))
		{
			bool		shadowed = strchr(z, '/') == NULL && sift_session_abbrev_not_utc(z);

			pfree(z);
			if (shadowed)
				return true;
		}
	}
	return expression_tree_walker(e, utc_shadowed_walker, arg);
}

/* the key's expression names UTC or GMT and the session zone gives the name another meaning: it is no step of the UTC clock in this session (a plan made in another zone) */
bool
sift_aggexpr_utc_shadowed(Node *e)
{
	return utc_shadowed_walker(e, NULL);
}

/* the integer, float or numeric cast of the number a field of a date was turned into: its value is a function of that number */
bool
is_num_cast(Node *e)
{
	static const char *const names[] = {"int2", "int4", "int8", "float4", "float8", "numeric"};
	FuncExpr   *f;
	char	   *name;
	bool		ok = false;

	if (!IsA(e, FuncExpr))
		return false;
	f = (FuncExpr *) e;
	if (list_length(f->args) != 1 || get_func_namespace(f->funcid) != PG_CATALOG_NAMESPACE || contain_volatile_functions(e))
		return false;
	name = get_func_name(f->funcid);
	for (int i = 0; i < lengthof(names); i++)
		ok |= strcmp(name, names[i]) == 0;
	return ok;
}

/* AT TIME ZONE 'UTC' of a timestamptz: the timestamp of the UTC clock */
bool
is_utc_conversion(Node *e)
{
	FuncExpr   *f;

	if (!IsA(e, FuncExpr))
		return false;
	f = (FuncExpr *) e;
	return list_length(f->args) == 2 && f->funcid == F_TIMEZONE_TEXT_TIMESTAMPTZ && utc_name(linitial(f->args));
}

static uint8
cal_range(int lo, int hi)
{
	uint8		m = 0;

	for (int g = Max(lo, SIFT_CAL_DAY); g <= Min(hi, SIFT_CAL_YEAR); g++)
		m |= CAL_BIT(g);
	return m;
}

/* the finest grain whose cells the result of the unit stays in (stay), and the coarsest it is constant over (res): 0 for a unit under a day */
static bool
cal_unit(const char *name, bool trunc, int *res, int *stay)
{
	static const struct
	{
		const char *name;
		int			res;
		int			stay;
	}			units[] = {{"microseconds", 0, 1}, {"milliseconds", 0, 1}, {"second", 0, 1}, {"minute", 0, 1}, {"hour", 0, 1}, {"day", 1, 1},
	{"week", 1, CAL_NONE}, {"month", 2, 2}, {"quarter", 2, 3}, {"year", 3, 3}, {"decade", 3, CAL_NONE}, {"century", 3, CAL_NONE},
	{"millennium", 3, CAL_NONE}, {"dow", 1, CAL_NONE}, {"isodow", 1, CAL_NONE}, {"doy", 1, CAL_NONE}, {"isoyear", 1, CAL_NONE}};

	for (int i = 0; i < lengthof(units); i++)
		if (pg_strcasecmp(units[i].name, name) == 0 && (!trunc || i <= 12))
		{
			*res = units[i].res;
			*stay = units[i].stay;
			return true;
		}
	return false;
}

static bool
cal_part(Node *e, Oid ptype, CalPart *out)
{
	FuncExpr   *f;
	CalPart		a;
	char	   *name;
	Oid			atype;
	int			res = 0,
				stay = CAL_NONE;
	int			nargs;
	bool		trunc,
				cast;

	if (IsA(e, Param) || IsA(e, Var))
	{
		*out = (CalPart) {ptype == DATEOID ? CAL_BIT(SIFT_CAL_DAY) : 0, SIFT_CAL_DAY, false, true};
		return IsA(e, Var) || ((Param *) e)->paramkind == PARAM_EXEC;
	}
	if (IsA(e, RelabelType))
		return cal_part((Node *) ((RelabelType *) e)->arg, ptype, out);
	if (!IsA(e, FuncExpr) || contain_volatile_functions(e))
		return false;
	f = (FuncExpr *) e;
	nargs = list_length(f->args);
	if (get_func_namespace(f->funcid) != PG_CATALOG_NAMESPACE)
		return false;
	name = get_func_name(f->funcid);
	trunc = strcmp(name, "date_trunc") == 0;
	cast = strcmp(name, "date") == 0 || strcmp(name, "timestamp") == 0 || strcmp(name, "timestamptz") == 0 || strcmp(name, "timezone") == 0;
	if (trunc || strcmp(name, "extract") == 0 || strcmp(name, "date_part") == 0)
	{
		Node	   *unit = linitial(f->args),
				   *arg = nargs > 1 ? lsecond(f->args) : NULL;
		bool		utc3 = nargs == 3 && trunc && utc_name(lthird(f->args));

		if (!(nargs == 2 || utc3) || !IsA(unit, Const) || ((Const *) unit)->constisnull || ((Const *) unit)->consttype != TEXTOID ||
			!cal_part(arg, ptype, &a) || !a.date)
			return false;
		name = TextDatumGetCString(((Const *) unit)->constvalue);
		if (!cal_unit(name, trunc, &res, &stay))
			return false;
		atype = exprType(arg);
		*out = (CalPart) {a.mask | cal_range(a.stay, res), trunc ? Max(a.stay, stay) : CAL_NONE, a.zoned || (atype == TIMESTAMPTZOID && !utc3), trunc};
		return true;
	}
	if (cast && nargs == 1 && !trunc)
	{
		if (!cal_part(linitial(f->args), ptype, &a))
			return false;
		atype = exprType(linitial(f->args));
		/* to a date: a function of the day of its argument; to a timestamp or timestamptz: the argument's own instants */
		*out = (CalPart) {a.mask | (strcmp(name, "date") == 0 ? cal_range(a.stay, 1) : 0), a.stay,
			a.zoned || atype == TIMESTAMPTZOID || strcmp(name, "timestamptz") == 0, true};
		return a.date && (strcmp(name, "date") == 0 || strcmp(name, "timestamp") == 0 || strcmp(name, "timestamptz") == 0);
	}
	if (is_utc_conversion(e))
	{
		if (!cal_part(lsecond(f->args), ptype, &a))
			return false;
		*out = a;
		return a.date;
	}
	/* the number a field of a date was turned into, or its integer or decimal cast: constant where the field is */
	if (is_num_cast(e))
	{
		if (!cal_part(linitial(f->args), ptype, &a) || a.date)
			return false;
		*out = a;
		return true;
	}
	return false;
}

/* the leaf type of a calendar expression: its Param 0 */
static bool
cal_ptype_walker(Node *n, Oid *ptype)
{
	if (n == NULL)
		return false;
	if (IsA(n, Param) && ((Param *) n)->paramkind == PARAM_EXEC)
	{
		*ptype = ((Param *) n)->paramtype;
		return true;
	}
	/* a key not compiled yet: its one column (single_leaf) is the leaf */
	if (IsA(n, Var))
	{
		*ptype = ((Var *) n)->vartype;
		return true;
	}
	return expression_tree_walker(n, cal_ptype_walker, ptype);
}

/* the grains every calendar part of a key or a test takes one value over; the rest of the tree is operators, tests and constants */
static bool
cal_tree(Node *e, Oid ptype, uint8 *mask, bool *zoned)
{
	CalPart		p;

	if (e == NULL || IsA(e, Const))
		return true;
	if (IsA(e, Param) || IsA(e, Var) || IsA(e, FuncExpr))
	{
		if (!cal_part(e, ptype, &p))
			return false;
		*mask &= p.mask;
		*zoned |= p.zoned;
		return true;
	}
	if (IsA(e, RelabelType))
		return cal_tree((Node *) ((RelabelType *) e)->arg, ptype, mask, zoned);
	if (IsA(e, OpExpr) || IsA(e, ScalarArrayOpExpr) || IsA(e, BoolExpr) || IsA(e, NullTest) || IsA(e, BooleanTest))
	{
		List	   *args = IsA(e, OpExpr) ? ((OpExpr *) e)->args : IsA(e, ScalarArrayOpExpr) ? ((ScalarArrayOpExpr *) e)->args :
			IsA(e, BoolExpr) ? ((BoolExpr *) e)->args : list_make1(IsA(e, NullTest) ? (Node *) ((NullTest *) e)->arg : (Node *) ((BooleanTest *) e)->arg);

		if (contain_volatile_functions(e))
			return false;
		foreach_ptr(Node, a, args)
			if (!cal_tree(a, ptype, mask, zoned))
				return false;
		return true;
	}
	return false;
}

int
sift_aggexpr_cal(Node *e, uint8 *mask)
{
	Oid			ptype = InvalidOid;
	bool		zoned = false;

	*mask = CAL_ALL;
	if (e == NULL || !cal_ptype_walker(e, &ptype) || (ptype != DATEOID && ptype != TIMESTAMPOID && ptype != TIMESTAMPTZOID) ||
		!cal_tree(e, ptype, mask, &zoned))
		return SAC_NOT;
	if (zoned)
		*mask &= ptype == DATEOID ? CAL_BIT(SIFT_CAL_DAY) : 0;
	if (*mask == 0)
		return zoned ? SAC_ZONE : SAC_FINE;
	return SAC_OK;
}

int
sift_aggkey_cal(const SiftAggKey *k, uint8 *mask)
{
	Oid			ptype = InvalidOid;

	*mask = 0;
	/* a key on a timestamp is evaluated for each day of its rows by the bucket: it has no other way to be evaluated for a cell */
	if (k->kind != SAK_LUT || k->expr == NULL || !cal_ptype_walker(k->expr, &ptype) || (ptype != DATEOID && k->bucket != DAY_SECS))
		return SAC_NOT;
	return sift_aggexpr_cal(k->expr, mask);
}

/*
 * The span of a leaf's values for an estimate: the smallest and largest value the index holds (its zones, rows deleted since included) when it has them,
 * else the histogram and most common values of pg_statistic, which a sample of the table made.
 */
bool
leaf_span(AggCtx *c, int leaf, int64 *lo, int64 *hi)
{
	if (leaf < SIFT_COLSRC_MAX_LEAVES && c->stats.ndistinct[leaf] > 0 && c->stats.max[leaf] >= c->stats.min[leaf] &&
		(c->stats.min[leaf] != 0 || c->stats.max[leaf] != 0))
	{
		*lo = (int64) c->stats.min[leaf];
		*hi = (int64) c->stats.max[leaf];
		return true;
	}
	return leaf_stats_range(c, leaf, lo, hi);
}

/* the groups of a bucket key from the statistics of its column; 0: unknown */
static double
bucket_key_groups(AggCtx *c, const SiftAggKey *k)
{
	int64		lo,
				hi;

	if (!leaf_span(c, k->leaf, &lo, &hi))
		return 0;
	return bucket_groups(c, k->expr, (double) (hi - lo) / USECS_PER_SEC);
}

/* the date_trunc, extract and date_part calls of a constant unit in an expression, the finest of them: for an estimate, whatever type the call is applied to */
static bool
calendar_units_walker(Node *n, CalUnits *cu)
{
	if (n == NULL)
		return false;
	if (IsA(n, FuncExpr) && get_func_namespace(((FuncExpr *) n)->funcid) == PG_CATALOG_NAMESPACE)
	{
		FuncExpr   *f = (FuncExpr *) n;
		char	   *fname = get_func_name(f->funcid);
		bool		trunc = strcmp(fname, "date_trunc") == 0;

		if ((trunc || strcmp(fname, "extract") == 0 || strcmp(fname, "date_part") == 0) && list_length(f->args) >= 2 && IsA(linitial(f->args), Const) &&
			!((Const *) linitial(f->args))->constisnull && ((Const *) linitial(f->args))->consttype == TEXTOID)
		{
			char	   *unit = TextDatumGetCString(((Const *) linitial(f->args))->constvalue);

			for (const BucketUnit *u = trunc ? trunc_units : field_units; u->name; u++)
				if (pg_strcasecmp(u->name, unit) == 0)
				{
					cu->n++;
					if (cu->finest == NULL || u->len < cu->finest->len)
						cu->finest = u;
				}
		}
	}
	return expression_tree_walker(n, calendar_units_walker, cu);
}

/*
 * The groups of a calendar key of a date leaf: the days the leaf's values take (PostgreSQL's distinct days under the table's conditions on it, no more
 * than the index's range of days), over the days one value of the key covers. PostgreSQL counts a function of a column as many groups as the column
 * has values. 0: not a calendar key of a date column, or the days are unknown.
 */
static double
date_key_groups(AggCtx *c, const SiftAggKey *k)
{
	SiftSrcLeaf *l;
	CalUnits	cu = {NULL, 0};
	Node	   *e;
	uint8		mask;
	double		days,
				n;

	if (k->kind != SAK_LUT || k->bucket > 0 || k->leaf < 0 || k->leaf >= c->lay->nleaves - c->nvleaf)
		return 0;
	l = leaf_of(c, k->leaf);
	if (l->kind != SCL_SCALAR || l->parent >= 0 || l->path != NIL || l->typid != DATEOID || sift_aggexpr_cal(k->expr, &mask) != SAC_OK)
		return 0;
	calendar_units_walker(k->expr, &cu);
	if (cu.finest == NULL)
		return 0;
	days = c->stats.ndistinct[k->leaf];
	e = list_nth(c->attexpr, l->attno - 1);
	if (e != NULL && IsA(e, Var))
	{
		double		nd = column_distinct(c, (Var *) e);

		if (nd > 0)
			days = days > 0 ? Min(days, nd) : nd;
	}
	if (days <= 0)
		return 0;
	n = Min(days, days / (cu.finest->len / DAY_SECS) + 1);
	return cu.n == 1 && cu.finest->cycle ? Min(n, cu.finest->cycle) : n;
}

/* the groups of a key whose number the planner knows: a calendar bucket from the statistics of its column, a calendar function of a date, a function of the JSON type of a path (at most seven); 0: unknown */
double
key_groups(AggCtx *c, const SiftAggKey *k)
{
	int			nbase = c->lay->nleaves - c->nvleaf;

	if ((k->kind == SAK_LEAF || k->kind == SAK_LUT) && k->leaf >= nbase && c->vleaf[k->leaf - nbase].types)
		return 7;
	return k->bucket > 0 ? bucket_key_groups(c, k) : date_key_groups(c, k);
}
