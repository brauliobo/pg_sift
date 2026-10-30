/*
 * agg/expr/walk.c - the walks over the expression of one leaf
 * What a leaf reads, whether an expression may be substituted or evaluated by the scan,
 * and the memo of walks over the outer query.
 */
#include "postgres.h"

#include "catalog/pg_statistic.h"
#include "nodes/nodeFuncs.h"
#include "optimizer/optimizer.h"
#include "parser/parsetree.h"
#include "utils/fmgroids.h"
#include "utils/lsyscache.h"
#include "utils/syscache.h"

#include "agg/agg.h"
#include "agg/expr/int.h"

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
 * The answers of the walks that look for the references to a query above (cte_owner_correlated, reads_outer) in the plan being made. The inline variant of a
 * statement repeats a CTE body at each of its references, and each of those walks goes through all of it again, once for each join of each arm, and into the
 * owner of every CTE it meets: the same answer 950 times over a plan of 14 G instructions. The answers hold while one planner run lasts (the nodes they are
 * keyed by are not freed before it ends); sift_agg_walks_forget drops them when a run starts and ends, as their addresses are used again afterwards.
 */
#define WALK_MEMO_MAX	4096

typedef struct WalkMemo
{
	const void *a,
			   *b;
	int			skip;
	bool		r;
} WalkMemo;

static WalkMemo *walk_memo;
static int	walk_memo_n;

#ifdef SIFT_TESTING
int64		sift_agg_outer_walks;	/* the walks made, not found in the memo */
int64		sift_agg_outer_asks;	/* the questions asked */
#endif

void
sift_agg_walks_forget(void)
{
	walk_memo_n = 0;
}

bool
walk_memo_get(const void *a, const void *b, int skip, bool *r)
{
#ifdef SIFT_TESTING
	sift_agg_outer_asks++;
#endif
	for (int i = 0; i < walk_memo_n; i++)
		if (walk_memo[i].a == a && walk_memo[i].b == b && walk_memo[i].skip == skip)
		{
			*r = walk_memo[i].r;
			return true;
		}
	return false;
}

void
walk_memo_put(const void *a, const void *b, int skip, bool r)
{
	if (walk_memo_n >= WALK_MEMO_MAX)
		return;
	if (walk_memo == NULL)
		walk_memo = MemoryContextAlloc(TopMemoryContext, WALK_MEMO_MAX * sizeof(WalkMemo));
	walk_memo[walk_memo_n++] = (WalkMemo) {a, b, skip, r};
}

/*
 * Does a CTE of this query level change with the rows of a query above it? One of the top level never does; one of a sub-select that reads
 * nothing of the queries around it (a WITH inside a derived table) does not either, but a lateral or correlated one does.
 */
bool
cte_owner_correlated(PlannerInfo *owner)
{
	OuterRef	oref = {owner, 0, 0};
	bool		r;

	if (owner->parent_root == NULL)
		return false;
	if (walk_memo_get(owner, NULL, 0, &r))
		return r;
#ifdef SIFT_TESTING
	sift_agg_outer_walks++;
#endif
	r = query_tree_walker(owner->parse, outer_ref_walker, &oref, QTW_EXAMINE_RTES_BEFORE);
	walk_memo_put(owner, NULL, 0, r);
	return r;
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
		s->c->sub_serial |= !((SubPlan *) n)->parallel_safe;
		s->c->subplan_cost += ((SubPlan *) n)->startup_cost;
	}
	else if (IsA(n, Param) && ((Param *) n)->paramkind == PARAM_EXEC && !null_input_param(n))
		foreach_ptr(SubPlan, sp, s->c->root->init_plans)
			if (list_member_int(sp->setParam, ((Param *) n)->paramid))
			{
				Param	   *init = copyObject((Param *) n);

				s->c->nsubplan++;
				s->c->sub_serial = true;
				s->c->subplan_cost += sp->startup_cost;
				init->paramid += SIFT_AGG_INIT_PARAM;
				return (Node *) init;
			}
	return expression_tree_mutator(n, subst_mutator, s);
}

Param *
exec_param(int id, Oid type, int32 typmod, Oid coll)
{
	Param	   *p = makeNode(Param);

	p->paramkind = PARAM_EXEC;
	p->paramid = id;
	p->paramtype = type;
	p->paramtypmod = typmod;
	p->paramcollid = coll;
	p->location = -1;
	return p;
}

/* e with the leaf reference replaced by Param 0 (PARAM_EXEC) of the type it had */
Node *
substitute_leaf(AggCtx *c, Node *e, Oid ptype, int32 ptypmod, Oid pcoll)
{
	SubstCtx	s = {c, exec_param(0, ptype, ptypmod, pcoll)};

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

/* date_trunc, date_part, extract and date() of a timestamptz read the clock of the session's time zone */
bool
session_zone_call(Oid funcid)
{
	return funcid == F_DATE_TRUNC_TEXT_TIMESTAMPTZ || funcid == F_DATE_PART_TEXT_TIMESTAMPTZ || funcid == F_EXTRACT_TEXT_TIMESTAMPTZ ||
		funcid == F_DATE_TIMESTAMPTZ;
}

