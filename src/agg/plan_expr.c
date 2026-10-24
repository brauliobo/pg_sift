/*
 * agg/plan_expr.c - expressions of one leaf: walkers, substitution, buckets and numeric programs
 */
#include "postgres.h"

#include <math.h>

#include "access/htup_details.h"
#include "access/sysattr.h"
#include "access/stratnum.h"
#include "access/table.h"
#include "access/stratnum.h"
#include "catalog/pg_aggregate.h"
#include "catalog/pg_am.h"
#include "catalog/pg_class.h"
#include "catalog/pg_collation.h"
#include "commands/defrem.h"
#include "catalog/pg_index.h"
#include "catalog/pg_namespace_d.h"
#include "miscadmin.h"
#include "catalog/pg_operator.h"
#include "catalog/pg_opfamily.h"
#include "catalog/pg_proc.h"
#include "catalog/pg_statistic.h"
#include "catalog/pg_type.h"
#include "nodes/extensible.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "optimizer/appendinfo.h"
#include "optimizer/clauses.h"
#include "optimizer/cost.h"
#include "optimizer/optimizer.h"
#include "optimizer/plancat.h"
#include "optimizer/pathnode.h"
#include "optimizer/paramassign.h"
#include "optimizer/paths.h"
#include "optimizer/planmain.h"
#include "optimizer/planner.h"
#include "optimizer/tlist.h"
#include "parser/parse_relation.h"
#include "parser/parsetree.h"
#include "port/pg_bitutils.h"
#include "rewrite/rewriteManip.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/fmgroids.h"
#include "utils/fmgrprotos.h"
#include "utils/jsonb.h"
#include "utils/jsonpath.h"
#include "utils/lsyscache.h"
#include "utils/numeric.h"
#include "utils/rel.h"
#include "utils/ruleutils.h"
#include "utils/selfuncs.h"
#include "utils/syscache.h"
#include "utils/typcache.h"

#include "agg/agg.h"
#include "agg/part.h"
#include "agg/topn.h"
#include "agg/spec.h"
#include "agg/colsrc.h"
#include "col/preagg.h"
#include "agg/search.h"
#include "agg/plan_int.h"

typedef struct LeafWalk
{
	AggCtx	   *c;
	int			leaf;			/* -1: none yet */
	Oid			ptype;			/* type of the references */
	int32		ptypmod;
	Oid			pcoll;
	bool		bad;
	List	   *subparams;		/* the output Params of the sub-plans met: the only executor parameters an expression may hold */
} LeafWalk;

/*
 * Does a CTE of this query level change with the rows of a query above it? One of the top level never does; one of a sub-select that reads
 * nothing of the queries around it (a WITH inside a derived table) does not either, but a lateral or correlated one does.
 */
bool
cte_owner_correlated(PlannerInfo *owner)
{
	OuterRef	oref = {owner, 0, 0};

	return owner->parent_root != NULL && query_tree_walker(owner->parse, outer_ref_walker, &oref, QTW_EXAMINE_RTES_BEFORE);
}

/*
 * Does the tree read a column or an aggregate of a query above it, or a common table expression of an enclosing subquery (a plan per
 * row, not a list)? A statement-level common table expression is read once wherever it is referenced.
 */
bool
outer_ref_walker(Node *n, OuterRef *o)
{
	if (n == NULL)
		return false;
	if (IsA(n, Var))
		return ((Var *) n)->varlevelsup > o->depth + o->skip;
	if (IsA(n, Aggref) && ((Aggref *) n)->agglevelsup > o->depth + o->skip)
		return true;
	if (IsA(n, RangeTblEntry))
	{
		RangeTblEntry *e = (RangeTblEntry *) n;
		PlannerInfo *owner = o->root;

		if (e->rtekind != RTE_CTE || e->ctelevelsup <= o->depth + o->skip)
			return false;
		for (int up = e->ctelevelsup - o->depth - 1; up > 0 && owner != NULL; up--)
			owner = owner->parent_root;
		return owner == NULL || cte_owner_correlated(owner);
	}
	if (IsA(n, Query))
	{
		bool		r;

		o->depth++;
		r = query_tree_walker((Query *) n, outer_ref_walker, o, QTW_EXAMINE_RTES_BEFORE);
		o->depth--;
		return r;
	}
	return expression_tree_walker(n, outer_ref_walker, o);
}

/* does the sub-select, a body of its own, read a column, aggregate or CTE of the queries around it? */
static bool
reads_outer_query(AggCtx *c, Query *q)
{
	OuterRef	oref = {c->root, 0, 0};

	return query_tree_walker(q, outer_ref_walker, &oref, QTW_EXAMINE_RTES_BEFORE);
}

/*
 * An uncorrelated ANY sub-plan that PostgreSQL hashes (x IN (SELECT ..), x NOT IN (SELECT ..) and their NULL rules): its answer is a function of
 * the tested value alone and holds for the statement, so the lookup table of the leaf may ask the sub-plan once per distinct value. The sub-select
 * is run when the scan starts, also when no row would reach it in the normal plan, so it must not raise, be volatile, lock rows, hold a sub-plan of its own
 * or read a column of an outer query (a CTE scan, an initplan of the outer level, a parameter of its plan): the groups are replayed for a rescan. The
 * extParam of its plan says that only after PostgreSQL has finished the whole plan, so the sub-select itself is walked, the copies of it that were planned too.
 */
static bool
subplan_runs_at_start(AggCtx *c, SubPlan *sp)
{
	PlannerGlobal *glob = c->root->glob;
	Query	   *q;

	if (sp->plan_id < 1 || sp->plan_id > list_length(glob->subroots))
		return false;
	q = ((PlannerInfo *) list_nth(glob->subroots, sp->plan_id - 1))->parse;
	return q->rowMarks == NIL && !contain_volatile_functions((Node *) q) && !raising_walker((Node *) q, c) && !reads_outer_query(c, q);
}

bool
lut_subplan_ok(AggCtx *c, SubPlan *sp)
{
	return sp->subLinkType == ANY_SUBLINK && sp->useHashTable && sp->testexpr != NULL && sp->parParam == NIL && sp->args == NIL &&
		sp->setParam == NIL && subplan_runs_at_start(c, sp);
}

/*
 * The output of an uncorrelated ARRAY(SELECT ..) of the query (x = ANY (ARRAY(SELECT ..)) has it as a parameter, set by an InitPlan the first time
 * the test needs it): the lookup table asks it once, under the same conditions as a hashed sub-plan. Another kind of initplan (a scalar
 * sub-select raises for a second row) or one of an outer query keeps the normal plan.
 */
bool
array_initplan_ok(AggCtx *c, Param *prm)
{
	if (null_input_param((Node *) prm))
		return false;
	foreach_ptr(SubPlan, sp, c->root->init_plans)
		if (list_member_int(sp->setParam, prm->paramid))
			return sp->subLinkType == ARRAY_SUBLINK && sp->parParam == NIL && sp->args == NIL && subplan_runs_at_start(c, sp);
	return false;
}

static bool
leaf_walker(Node *n, LeafWalk *w)
{
	int			id;

	if (n == NULL || w->bad)
		return false;
	if (null_input_param(n))
	{
		w->bad = true;
		return false;
	}
	if (IsA(n, Var) || IsA(n, CoerceViaIO) || IsA(n, FuncExpr) || IsA(n, OpExpr) || IsA(n, RelabelType))
	{
		if ((id = match_leaf(w->c, n)) >= 0)
		{
			if (w->leaf >= 0 && (w->leaf != id || w->ptype != exprType(n)))
				w->bad = true;
			w->leaf = id;
			w->ptype = exprType(n);
			w->ptypmod = exprTypmod(n);
			w->pcoll = exprCollation(n);
			return false;
		}
		if (IsA(n, Var))
		{
			w->bad = true;		/* a column the source does not hold in this form */
			return false;
		}
	}
	if (IsA(n, SubPlan) && lut_subplan_ok(w->c, (SubPlan *) n))
		w->subparams = list_concat_copy(w->subparams, ((SubPlan *) n)->paramIds);
	else if (IsA(n, Param) && ((Param *) n)->paramkind == PARAM_EXEC && !list_member_int(w->subparams, ((Param *) n)->paramid) &&
			 !array_initplan_ok(w->c, (Param *) n))
	{
		w->bad = true;
		return false;
	}
	else if (IsA(n, SubPlan) || IsA(n, SubLink) || IsA(n, Aggref) || IsA(n, WindowFunc) || IsA(n, GroupingFunc) ||
		IsA(n, PlaceHolderVar) || IsA(n, CurrentOfExpr) || IsA(n, NextValueExpr) || IsA(n, SQLValueFunction) ||
		IsA(n, MinMaxExpr) || IsA(n, SetToDefault))
	{
		if (!IsA(n, MinMaxExpr) && !IsA(n, SQLValueFunction))
		{
			w->bad = true;
			return false;
		}
	}
	return expression_tree_walker(n, leaf_walker, w);
}

/* the one leaf an expression reads (and the type it reads it as); false if it reads none, several, or something else */
bool
single_leaf(AggCtx *c, Node *e, int *leaf, Oid *ptype, int32 *ptypmod, Oid *pcoll)
{
	LeafWalk	w = {c, -1, InvalidOid, -1, InvalidOid, false, NIL};

	if (contain_volatile_functions(e))
		return false;
	leaf_walker(e, &w);
	if (w.bad || w.leaf < 0)
		return false;
	*leaf = w.leaf;
	*ptype = w.ptype;
	*ptypmod = w.ptypmod;
	*pcoll = w.pcoll;
	return true;
}

typedef struct SubstCtx
{
	AggCtx	   *c;
	Param	   *param;
} SubstCtx;

/*
 * PostgreSQL hashes the array of x = ANY (array) when it is a constant; one that comes from a parameter (or an ARRAY sub-select) is searched linearly for every
 * value the executor tests (a dictionary of 200k entries against 5000 elements takes seconds). The executor builds the hash table on the
 * first call and keeps it, so the parameter is read once.
 */
void
hash_param_saop(ScalarArrayOpExpr *sa)
{
	Node	   *arr = lsecond(sa->args);
	Oid			op = sa->opno,
				lh,
				rh;

	if (!IsA(arr, Param) || (((Param *) arr)->paramkind != PARAM_EXTERN && ((Param *) arr)->paramid < SIFT_AGG_INIT_PARAM) || OidIsValid(sa->hashfuncid))
		return;
	if (!sa->useOr && !OidIsValid(op = get_negator(op)))
		return;
	if (!get_op_hash_functions(op, &lh, &rh) || lh != rh)
		return;
	sa->hashfuncid = lh;
	sa->negfuncid = sa->useOr ? InvalidOid : get_opcode(op);
}

static Node *
subst_mutator(Node *n, SubstCtx *s)
{
	if (n == NULL)
		return NULL;
	if (IsA(n, ScalarArrayOpExpr))
	{
		Node	   *r = expression_tree_mutator(n, subst_mutator, s);

		hash_param_saop((ScalarArrayOpExpr *) r);
		return r;
	}
	if ((IsA(n, Var) || IsA(n, CoerceViaIO) || IsA(n, FuncExpr) || IsA(n, OpExpr) || IsA(n, RelabelType)) &&
		match_leaf(s->c, n) >= 0)
		return (Node *) copyObject(s->param);
	if (IsA(n, SubPlan))
	{
		s->c->nsubplan++;
		s->c->subplan_cost += ((SubPlan *) n)->startup_cost;
	}
	else if (IsA(n, Param) && ((Param *) n)->paramkind == PARAM_EXEC && !null_input_param(n))
		foreach_ptr(SubPlan, sp, s->c->root->init_plans)
			if (list_member_int(sp->setParam, ((Param *) n)->paramid))
			{
				Param	   *init = copyObject((Param *) n);

				s->c->nsubplan++;
				s->c->subplan_cost += sp->startup_cost;
				init->paramid += SIFT_AGG_INIT_PARAM;
				return (Node *) init;
			}
	return expression_tree_mutator(n, subst_mutator, s);
}

/* e with the leaf reference replaced by Param 0 (PARAM_EXEC) of the type it had */
Node *
substitute_leaf(AggCtx *c, Node *e, Oid ptype, int32 ptypmod, Oid pcoll)
{
	Param	   *p = makeNode(Param);
	SubstCtx	s = {c, p};

	p->paramkind = PARAM_EXEC;
	p->paramid = 0;
	p->paramtype = ptype;
	p->paramtypmod = ptypmod;
	p->paramcollid = pcoll;
	p->location = -1;
	return subst_mutator(e, &s);
}

bool
lut_leaf_ok(AggCtx *c, int leaf)
{
	SiftSrcLeaf *l = leaf_of(c, leaf);

	return l->kind == SCL_SCALAR && !l->wide && (l->vtype == SCV_CODE || (l->vtype == SCV_I32 && !type_is_enum(l->typid)));
}

/* a leaf an expression PostgreSQL evaluates may read: a combo gives an id to each distinct value of a dictionary, integer, 64-bit integer (numeric included) or float leaf */
bool
combo_leaf_ok(AggCtx *c, int leaf)
{
	SiftSrcLeaf *l = leaf_of(c, leaf);

	return l->kind == SCL_SCALAR && (l->vtype == SCV_CODE || l->vtype == SCV_I32 || l->vtype == SCV_I64 || l->vtype == SCV_F64);
}

/* an integer division or remainder by a constant other than 0 and -1 (the overflow of the smallest value) cannot raise */
static bool
safe_int_divisor(const char *name, List *args)
{
	Const	   *k;

	if (strncmp(name, "int", 3) != 0 || (!strstr(name, "div") && !strstr(name, "mod")) || list_length(args) != 2)
		return false;
	k = (Const *) lsecond(args);
	if (!IsA(k, Const) || k->constisnull || (k->consttype != INT2OID && k->consttype != INT4OID && k->consttype != INT8OID))
		return false;
	return k->consttype == INT2OID ? DatumGetInt16(k->constvalue) != 0 && DatumGetInt16(k->constvalue) != -1 :
		k->consttype == INT4OID ? DatumGetInt32(k->constvalue) != 0 && DatumGetInt32(k->constvalue) != -1 :
		DatumGetInt64(k->constvalue) != 0 && DatumGetInt64(k->constvalue) != -1;
}

/*
 * Functions that raise an error for some values of their input (a zero divisor, a negative root, a text that is no number). A lookup table
 * evaluates its expression on every value of the dictionary or of the range of the unit, also on values no selected row has; the combos
 * evaluate it for the values of the tuples that reach it (docs/COL-PLAN.md, "Expressions PostgreSQL evaluates").
 */
bool
raising_walker(Node *n, void *context)
{
	static const char *const names[] = {"int2div", "int4div", "int8div", "int24div", "int42div", "int28div", "int82div", "int48div", "int84div",
		"int2mod", "int4mod", "int8mod", "numeric_div", "numeric_mod", "numeric_div_trunc", "float8div", "float4div", "float48div",
		"float84div", "dpow", "numeric_power", "dlog1", "dlog10", "numeric_ln", "numeric_log", "dsqrt", "numeric_sqrt", "dcbrt", "acos",
		"asin", "acosh", "atanh", "trunc_div"};
	Oid			fid = InvalidOid;
	List	   *args = NIL;

	if (n == NULL)
		return false;
	if (IsA(n, Query))
		return query_tree_walker((Query *) n, raising_walker, context, 0);
	/* a context: a sub-select, whose own sub-plans and init plans may raise too (more than one row) */
	if (IsA(n, CoerceViaIO) || (context && (IsA(n, SubPlan) || IsA(n, SubLink) || (IsA(n, Param) && ((Param *) n)->paramkind == PARAM_EXEC))))
		return true;
	if (IsA(n, FuncExpr))
	{
		fid = ((FuncExpr *) n)->funcid;
		args = ((FuncExpr *) n)->args;
	}
	else if (IsA(n, OpExpr))
	{
		set_opfuncid((OpExpr *) n);
		fid = ((OpExpr *) n)->opfuncid;
		args = ((OpExpr *) n)->args;
	}
	if (OidIsValid(fid))
	{
		const char *name = get_func_name(fid);

		for (int i = 0; name && i < lengthof(names); i++)
			if (strcmp(name, names[i]) == 0 && !safe_int_divisor(name, args))
				return true;
	}
	return expression_tree_walker(n, raising_walker, context);
}

/* the span of the values the statistics of an integer column (histogram, most common values) have seen; false: none, or the leaf is not a plain column */
bool
leaf_stats_range(AggCtx *c, int leaf, int64 *lo, int64 *hi)
{
	SiftSrcLeaf *l = leaf_of(c, leaf);
	Node	   *e = l->path == NIL && l->parent < 0 ? list_nth(c->attexpr, l->attno - 1) : NULL;
	HeapTuple	tp;
	AttStatsSlot sslot;
	bool		known = false;

	*lo = PG_INT64_MAX;
	*hi = PG_INT64_MIN;
	if (e == NULL || !IsA(e, Var))
		return false;
	tp = SearchSysCache3(STATRELATTINH, ObjectIdGetDatum(planner_rt_fetch(c->relid, c->root)->relid),
						 Int16GetDatum(((Var *) e)->varattno), BoolGetDatum(false));
	if (!HeapTupleIsValid(tp))
		return false;
	for (int k = 0; k < 2; k++)
		if (get_attstatsslot(&sslot, tp, k == 0 ? STATISTIC_KIND_HISTOGRAM : STATISTIC_KIND_MCV, InvalidOid, ATTSTATSSLOT_VALUES))
		{
			for (int i = 0; i < sslot.nvalues; i++)
			{
				int64		v = sift_aggspec_operand_int(l->typid, sslot.values[i]);

				*lo = Min(*lo, v);
				*hi = Max(*hi, v);
				known = true;
			}
			free_attstatsslot(&sslot);
		}
	ReleaseSysCache(tp);
	return known;
}

/* the values of an integer leaf the lookup table covers: the span of the column's statistics fits, or there are none */
static bool
lut_range_ok(AggCtx *c, int leaf)
{
	int64		lo,
				hi;

	return !leaf_stats_range(c, leaf, &lo, &hi) || (double) hi - (double) lo < (double) (1 << 22);
}

/* is an expression of one dictionary or integer leaf evaluated by a lookup table (once per value of the dictionary or of the range of the leaf)? */
bool
lut_expr_ok(AggCtx *c, int leaf, Node *expr)
{
	return lut_leaf_ok(c, leaf) && !raising_walker(expr, NULL) && (leaf_of(c, leaf)->vtype == SCV_CODE || lut_range_ok(c, leaf));
}

/*
 * Calendar keys of a timestamp leaf. date_trunc, ::date and extract of a timestamp change value only on a second, minute, hour or
 * day boundary, so the executor evaluates them once per such step of the data instead of once per distinct timestamp (which is
 * once per row): the key id of a row is a lookup of its step.
 */
typedef struct BucketUnit
{
	const char *name;
	int			secs;			/* the step the result changes on */
	double		len;			/* average seconds of a unit */
	int			cycle;			/* extract: the values it repeats after, 0: it does not */
} BucketUnit;

#define DAY_SECS		86400
#define MONTH_SECS		2629800
#define YEAR_SECS		31557600.0

/* date_trunc units (a week, month and longer ones change on a day) */
static const BucketUnit trunc_units[] = {{"second", 1, 1, 0}, {"minute", 60, 60, 0}, {"hour", 3600, 3600, 0}, {"day", DAY_SECS, DAY_SECS, 0},
	{"week", DAY_SECS, 7 * DAY_SECS, 0}, {"month", DAY_SECS, MONTH_SECS, 0}, {"quarter", DAY_SECS, 3 * MONTH_SECS, 0},
	{"year", DAY_SECS, YEAR_SECS, 0}, {"decade", DAY_SECS, 10 * YEAR_SECS, 0}, {"century", DAY_SECS, 100 * YEAR_SECS, 0},
	{"millennium", DAY_SECS, 1000 * YEAR_SECS, 0}, {NULL, 0, 0, 0}};

/* extract fields that are whole numbers of a step (second, epoch and julian have fractions of a second) */
static const BucketUnit field_units[] = {{"minute", 60, 60, 60}, {"hour", 3600, 3600, 24}, {"day", DAY_SECS, DAY_SECS, 31},
	{"dow", DAY_SECS, DAY_SECS, 7}, {"isodow", DAY_SECS, DAY_SECS, 7}, {"doy", DAY_SECS, DAY_SECS, 366}, {"week", DAY_SECS, 7 * DAY_SECS, 53},
	{"month", DAY_SECS, MONTH_SECS, 12}, {"quarter", DAY_SECS, 3 * MONTH_SECS, 4}, {"year", DAY_SECS, YEAR_SECS, 0},
	{"isoyear", DAY_SECS, YEAR_SECS, 0}, {"decade", DAY_SECS, 10 * YEAR_SECS, 0}, {"century", DAY_SECS, 100 * YEAR_SECS, 0},
	{"millennium", DAY_SECS, 1000 * YEAR_SECS, 0}, {NULL, 0, 0, 0}};

static bool utc_name(Node *e);

/* the unit of a date_trunc, date_part or extract of a timestamp; NULL for another call, or a unit this does not know */
static const BucketUnit *
bucket_unit(Node *e)
{
	FuncExpr   *f;
	Const	   *k;
	char	   *name;
	bool		trunc;

	if (!IsA(e, FuncExpr))
		return NULL;
	f = (FuncExpr *) e;
	/* date_trunc(unit, timestamptz, 'UTC') cuts the UTC clock, the same in every session */
	trunc = f->funcid == F_DATE_TRUNC_TEXT_TIMESTAMP || (f->funcid == F_DATE_TRUNC_TEXT_TIMESTAMPTZ_TEXT && utc_name(lthird(f->args)));
	if (!trunc && f->funcid != F_DATE_PART_TEXT_TIMESTAMP && f->funcid != F_EXTRACT_TEXT_TIMESTAMP)
		return NULL;
	k = linitial(f->args);
	if (!IsA(k, Const) || k->constisnull || k->consttype != TEXTOID)
		return NULL;
	name = TextDatumGetCString(k->constvalue);
	for (const BucketUnit *u = trunc ? trunc_units : field_units; u->name; u++)
		if (pg_strcasecmp(u->name, name) == 0)
			return u;
	return NULL;
}

static bool is_utc_conversion(Node *e);
static bool is_num_cast(Node *e);

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
bucket_step(AggCtx *c, Node *e)
{
	const BucketUnit *u;
	int			arg;

	if (bucket_leaf(c, e))
		return 1;
	if (IsA(e, RelabelType))
		return bucket_step(c, (Node *) ((RelabelType *) e)->arg);
	if (!IsA(e, FuncExpr))
		return 0;
	if (is_utc_conversion(e))
		return bucket_step(c, lsecond(((FuncExpr *) e)->args));
	if (is_num_cast(e))
		return bucket_step(c, linitial(((FuncExpr *) e)->args));
	if ((u = bucket_unit(e)) != NULL)
	{
		arg = bucket_step(c, lsecond(((FuncExpr *) e)->args));
		return arg ? Max(arg, u->secs) : 0;
	}
	switch (((FuncExpr *) e)->funcid)
	{
		case F_DATE_TIMESTAMP:
			arg = bucket_step(c, linitial(((FuncExpr *) e)->args));
			return arg ? Max(arg, DAY_SECS) : 0;
		case F_TIMESTAMP_DATE:
			return bucket_step(c, linitial(((FuncExpr *) e)->args));
		default:
			return 0;
	}
}

/* the distinct values an expression that bucket_step accepts takes over a stretch of `span` seconds of the leaf */
static double
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
	if ((u = bucket_unit(e)) != NULL)
	{
		n = Min(bucket_groups(c, lsecond(((FuncExpr *) e)->args), span), span / u->len + 1);
		return u->cycle ? Min(n, u->cycle) : n;
	}
	if (((FuncExpr *) e)->funcid == F_DATE_TIMESTAMP)
		return Min(bucket_groups(c, linitial(((FuncExpr *) e)->args), span), span / DAY_SECS + 1);
	return bucket_groups(c, linitial(((FuncExpr *) e)->args), span);
}

/* a time zone name of UTC: the three-argument date_trunc and AT TIME ZONE with it give the same value in every session */
static bool
utc_name(Node *e)
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
	pfree(z);
	return ok;
}

/* the integer, float or numeric cast of the number a field of a date was turned into: its value is a function of that number */
static bool
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
static bool
is_utc_conversion(Node *e)
{
	FuncExpr   *f;

	if (!IsA(e, FuncExpr))
		return false;
	f = (FuncExpr *) e;
	return list_length(f->args) == 2 && f->funcid == F_TIMEZONE_TEXT_TIMESTAMPTZ && utc_name(linitial(f->args));
}

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
	/* a key on a timestamp is evaluated for each day of its rows by the bucket (docs/COL.md 2.13): it has no other way to be evaluated for a cell */
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

typedef struct CalUnits
{
	const BucketUnit *finest;
	int			n;
} CalUnits;

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

/* EXPLAIN: the step a key on a timestamp is evaluated for */
const char *
bucket_text(int secs)
{
	return secs == 1 ? " (per second)" : secs == 60 ? " (per minute)" : secs == 3600 ? " (per hour)" : " (per day)";
}

/* the seconds a key on the timestamp leaf is evaluated once for, 0 when it is not a calendar expression */
int
bucket_key_secs(AggCtx *c, int leaf, Node *expr)
{
	SiftSrcLeaf *l = leaf_of(c, leaf);

	uint8		mask;

	if (l->kind != SCL_SCALAR || l->vtype != SCV_I64 || (l->typid != TIMESTAMPOID && l->typid != TIMESTAMPTZOID) || contain_volatile_functions(expr))
		return 0;
	/* a timestamptz key is the same in every session only if it reads the UTC clock (AT TIME ZONE 'UTC'): the others have no bucket */
	if (l->typid == TIMESTAMPTZOID && sift_aggexpr_cal(expr, &mask) != SAC_OK)
		return 0;
	return bucket_step(c, expr);
}

/* digits an integer or numeric(p, s) column may hold: from the type modifier of its reference, else what the storage holds */
int
leaf_digits(Oid typid, int32 typmod)
{
	if (typid == INT2OID)
		return 5;
	if (typid == INT4OID)
		return 10;
	return typmod >= 0 ? ((typmod - VARHDRSZ) >> 16) & 0xffff : 19;
}

static bool
nx_put(SiftAggNum *o, SiftAggNxOp op, int leaf, int a, int b, sift_i128 c)
{
	if (o->n >= SIFT_AGG_MAX_VX)
		return false;
	o->step[o->n++] = (SiftAggNx) {op, leaf, a, b, c};
	return true;
}

static bool
nx_leaf(AggCtx *c, Node *n, int leaf, SiftAggNum *o, int *scale, int *dig)
{
	SiftSrcLeaf *l = leaf_of(c, leaf);

	if (l->kind != SCL_SCALAR || !((l->vtype == SCV_I64 && (l->typid == NUMERICOID || l->typid == INT8OID)) ||
								   (l->vtype == SCV_I32 && (l->typid == INT2OID || l->typid == INT4OID))))
		return false;
	use_leaf(c, leaf);
	*scale = l->typid == NUMERICOID ? l->scale : 0;
	*dig = leaf_digits(l->typid, exprTypmod(n));
	return nx_put(o, SNX_LEAF, leaf, 0, 0, 0);
}

/* a constant is its text: the digits and the dscale PostgreSQL prints; NaN and the infinities are not numbers of the program */
static bool
nx_const(Const *k, SiftAggNum *o, int *scale, int *dig)
{
	sift_i128	v,
				m;

	if (k->constisnull || k->consttype != NUMERICOID ||
		!sift_parse_decimal(DatumGetCString(DirectFunctionCall1(numeric_out, k->constvalue)), &v, scale))
		return false;
	for (*dig = 1, m = v < 0 ? -v : v; m >= 10; m /= 10)
		(*dig)++;
	return nx_put(o, SNX_CONST, -1, 0, 0, v);
}

/* left op right, the result scale the larger one (numeric_add, numeric_sub); *scale and *dig are the left operand's on entry */
bool
nx_addsub(SiftAggNum *o, SiftAggNxOp op, int *scale, int *dig, int s2, int d2)
{
	int			s1 = *scale,
				sc = Max(s1, s2);

	*dig = Max(*dig + sc - s1, d2 + sc - s2) + 1;
	*scale = sc;
	return *dig <= SIFT_NUM_MAX_DIGITS && nx_put(o, op, -1, sc - s1, sc - s2, 0);
}

/* round, trunc, floor, ceil to n digits after the point: the result has scale n (0 for a negative n), as numeric_round and its kin give */
static bool
nx_rescale(SiftAggNum *o, SiftAggNxOp op, int n, int *scale, int *dig)
{
	int			a = Max(*scale - n, 0),
				b = n >= *scale ? n - *scale : Max(-n, 0);

	*dig = (a > 0 ? Max(*dig - a, 0) + 1 : *dig) + b;
	*scale = Max(n, 0);
	return *dig <= SIFT_NUM_MAX_DIGITS && nx_put(o, op, -1, a, b, 0);
}

/*
 * The program of a numeric expression: leaves, numeric constants, + - * and unary minus, abs, round / trunc / floor / ceil with a constant
 * number of digits, the casts of integers to numeric and of numeric to an integer (the digits it needs fit the integer: no overflow
 * error to reproduce). Anything else (division, other functions, parameters inside the arithmetic, a result of more than
 * SIFT_NUM_MAX_DIGITS digits) is false: its scale or its errors are not known here.
 */
bool
nx_walk(AggCtx *c, Node *n, SiftAggNum *o, int *scale, int *dig)
{
	int			leaf,
				s2,
				d2,
				nd = 0,
				maxd = SIFT_NUM_MAX_DIGITS;
	Oid			fid;
	List	   *args;
	SiftAggNxOp op;

	n = strip_relabel(n);
	if (IsA(n, Const))
		return nx_const((Const *) n, o, scale, dig);
	if ((leaf = match_leaf(c, n)) >= 0)
		return nx_leaf(c, n, leaf, o, scale, dig);
	fid = func_of(n);
	args = func_args(n);
	switch (fid)
	{
		case F_NUMERIC_ADD:
		case F_NUMERIC_SUB:
		case F_NUMERIC_MUL:
			if (!nx_walk(c, linitial(args), o, scale, dig) || !nx_walk(c, lsecond(args), o, &s2, &d2))
				return false;
			if (fid != F_NUMERIC_MUL)
				return nx_addsub(o, fid == F_NUMERIC_ADD ? SNX_ADD : SNX_SUB, scale, dig, s2, d2);
			*scale += s2;
			*dig += d2;
			return *dig <= SIFT_NUM_MAX_DIGITS && nx_put(o, SNX_MUL, -1, 0, 0, 0);
		case F_NUMERIC_UMINUS:
		case F_ABS_NUMERIC:
		case F_NUMERIC_ABS:
			return nx_walk(c, linitial(args), o, scale, dig) && nx_put(o, fid == F_NUMERIC_UMINUS ? SNX_NEG : SNX_ABS, -1, 0, 0, 0);
		case F_NUMERIC_INT2:
		case F_NUMERIC_INT4:
		case F_NUMERIC_INT8:
			return nx_walk(c, linitial(args), o, scale, dig);
		case F_INT2_NUMERIC:
		case F_INT4_NUMERIC:
		case F_INT8_NUMERIC:
			maxd = fid == F_INT2_NUMERIC ? 4 : fid == F_INT4_NUMERIC ? 9 : 18;
			return nx_walk(c, linitial(args), o, scale, dig) && nx_rescale(o, SNX_ROUND, 0, scale, dig) && *dig <= maxd &&
				nx_put(o, SNX_INTCAST, -1, fid == F_INT2_NUMERIC ? 2 : fid == F_INT4_NUMERIC ? 4 : 8, 0, 0);
		case F_ROUND_NUMERIC:
		case F_ROUND_NUMERIC_INT4:
		case F_TRUNC_NUMERIC:
		case F_TRUNC_NUMERIC_INT4:
		case F_FLOOR_NUMERIC:
		case F_CEIL_NUMERIC:
		case F_CEILING_NUMERIC:
			if (list_length(args) == 2)
			{
				Const	   *k = (Const *) strip_relabel(lsecond(args));

				if (!IsA(k, Const) || k->constisnull || abs(nd = DatumGetInt32(k->constvalue)) > SIFT_NUM_MAX_DIGITS)
					return false;
			}
			op = (fid == F_ROUND_NUMERIC || fid == F_ROUND_NUMERIC_INT4) ? SNX_ROUND :
				(fid == F_TRUNC_NUMERIC || fid == F_TRUNC_NUMERIC_INT4) ? SNX_TRUNC : fid == F_FLOOR_NUMERIC ? SNX_FLOOR : SNX_CEIL;
			return nx_walk(c, linitial(args), o, scale, dig) && nx_rescale(o, op, nd, scale, dig);
		default:
			return false;
	}
}

/* the program of e, at most maxdig digits wide */
bool
compile_nx(AggCtx *c, Node *e, int maxdig, SiftAggNum *o)
{
	int			dig;

	o->n = 0;
	o->step = palloc(SIFT_AGG_MAX_VX * sizeof(SiftAggNx));
	return nx_walk(c, e, o, &o->scale, &dig) && dig <= maxdig;
}

/* + - * of int8, int4 and int2 operands (int8pl, int84pl, int48pl ...): the SNX op of the function, -1 for any other */
static int
int8_arith_op(Oid fid)
{
	switch (fid)
	{
		case F_INT8PL:
		case F_INT84PL:
		case F_INT82PL:
		case F_INT48PL:
		case F_INT28PL:
			return SNX_ADD;
		case F_INT8MI:
		case F_INT84MI:
		case F_INT82MI:
		case F_INT48MI:
		case F_INT28MI:
			return SNX_SUB;
		case F_INT8MUL:
		case F_INT84MUL:
		case F_INT82MUL:
		case F_INT48MUL:
		case F_INT28MUL:
			return SNX_MUL;
		default:
			return -1;
	}
}

/*
 * `leaf OP constant` or `constant OP leaf` of an int8 column, OP being + - *: the program ends in a cast to int8 that raises "bigint out of range" for
 * a result outside it, as int8pl and its kin do. The rows the cast looks at are the aggregate's (eval_nx).
 */
bool
compile_int8_arith(AggCtx *c, Node *e, SiftAggNum *o)
{
	Node	   *e1 = strip_relabel(e);
	int			op = int8_arith_op(func_of(e1)),
				leaf = -1,
				k = -1;
	List	   *args = func_args(e1);

	if (op < 0 || list_length(args) != 2)
		return false;
	for (int i = 0; i < 2; i++)
	{
		Node	   *a = strip_relabel(list_nth(args, i));
		Const	   *kc = (Const *) a;

		if (IsA(a, Const))
		{
			if (kc->constisnull || (kc->consttype != INT8OID && kc->consttype != INT4OID && kc->consttype != INT2OID) || k >= 0)
				return false;
			k = i;
		}
		else if ((leaf = match_leaf(c, a)) < 0)
			return false;
	}
	if (k < 0 || leaf < 0 || leaf_of(c, leaf)->kind != SCL_SCALAR || leaf_of(c, leaf)->vtype != SCV_I64 || leaf_of(c, leaf)->typid != INT8OID ||
		leaf_of(c, leaf)->wide)
		return false;
	{
		Const	   *kc = (Const *) strip_relabel(list_nth(args, k));
		int64		v = kc->consttype == INT8OID ? DatumGetInt64(kc->constvalue) : kc->consttype == INT4OID ? DatumGetInt32(kc->constvalue) : DatumGetInt16(kc->constvalue);

		o->n = 0;
		o->scale = 0;
		o->step = palloc(SIFT_AGG_MAX_VX * sizeof(SiftAggNx));
		use_leaf(c, leaf);
		return (k == 0 ? nx_put(o, SNX_CONST, -1, 0, 0, v) && nx_put(o, SNX_LEAF, leaf, 0, 0, 0) : nx_put(o, SNX_LEAF, leaf, 0, 0, 0) && nx_put(o, SNX_CONST, -1, 0, 0, v)) &&
			nx_put(o, op, -1, 0, 0, 0) && nx_put(o, SNX_INTCAST, -1, 8, 1, 0);
	}
}

/* the level (0 row, 1 element) of the leaves of a program, -1 when it mixes them */
int
nx_level(AggCtx *c, const SiftAggNum *o)
{
	int			lvl = -1;

	for (int k = 0; k < o->n; k++)
		if (o->step[k].op == SNX_LEAF)
		{
			int			l = leaf_level(c, o->step[k].leaf);

			if (lvl >= 0 && l != lvl)
				return -1;
			lvl = l;
		}
	return lvl;
}

/* does the program raise for a NaN (a cast to an integer)? */
bool
nx_raises(const SiftAggNum *o)
{
	for (int k = 0; k < o->n; k++)
		if (o->step[k].op == SNX_INTCAST)
			return true;
	return false;
}
