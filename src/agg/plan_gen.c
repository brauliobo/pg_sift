/*
 * agg/plan_gen.c - generic aggregates: PostgreSQL's own transition and final functions (SAA_GEN)
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
#include "agg/search.h"
#include "agg/plan_int.h"

/* ---------- generic aggregates: PostgreSQL's own transition and final functions (SAA_GEN) ---------- */

/* a leaf the executor can turn into a Datum of its type */
static bool
gen_leaf_ok(AggCtx *c, int id)
{
	SiftSrcLeaf *l = leaf_of(c, id);

	if (l->kind != SCL_SCALAR || l->lossy)
		return false;
	if (l->wide)
		return true;
	switch (l->typid)
	{
		case BOOLOID:
		case INT2OID:
		case INT4OID:
		case DATEOID:
		case INT8OID:
		case TIMESTAMPOID:
		case TIMESTAMPTZOID:
		case FLOAT4OID:
		case FLOAT8OID:
		case TEXTOID:
		case VARCHAROID:
			return true;
		case CASHOID:
			return l->vtype == SCV_I64;
		case NUMERICOID:
			return l->vtype == SCV_I64;
		case TIMEOID:
			return l->vtype == SCV_I64;
		default:
			return l->vtype == SCV_I32 && type_is_enum(l->typid);
	}
}

typedef struct GenWalk
{
	AggCtx	   *c;
	int			nl;
	int			leaf[SIFT_AGG_MAX_GLEAVES];
	Oid			ptype[SIFT_AGG_MAX_GLEAVES];
	int32		ptypmod[SIFT_AGG_MAX_GLEAVES];
	Oid			pcoll[SIFT_AGG_MAX_GLEAVES];
	bool		bad;
} GenWalk;

/* the leaves an argument expression reads (each with one type), and whether it reads anything the executor cannot bind */
static bool
gen_walker(Node *n, GenWalk *w)
{
	int			id,
				p = 0;

	if (n == NULL || w->bad)
		return false;
	if (IsA(n, Var) || IsA(n, CoerceViaIO) || IsA(n, FuncExpr) || IsA(n, OpExpr) || IsA(n, RelabelType))
	{
		if ((id = match_leaf(w->c, n)) >= 0)
		{
			while (p < w->nl && w->leaf[p] != id)
				p++;
			if (p == SIFT_AGG_MAX_GLEAVES || !gen_leaf_ok(w->c, id) ||
				(p < w->nl && (w->ptype[p] != exprType(n) || w->ptypmod[p] != exprTypmod(n) || w->pcoll[p] != exprCollation(n))))
				w->bad = true;
			else if (p == w->nl)
			{
				w->leaf[p] = id;
				w->ptype[p] = exprType(n);
				w->ptypmod[p] = exprTypmod(n);
				w->pcoll[p] = exprCollation(n);
				w->nl++;
			}
			return false;
		}
		w->bad |= IsA(n, Var);
	}
	if ((IsA(n, Param) && ((Param *) n)->paramkind == PARAM_EXEC) || IsA(n, SubPlan) || IsA(n, SubLink) || IsA(n, Aggref) ||
		IsA(n, WindowFunc) || IsA(n, GroupingFunc) || IsA(n, PlaceHolderVar) || IsA(n, CurrentOfExpr) || IsA(n, NextValueExpr) ||
		IsA(n, SetToDefault))
		w->bad = true;
	return !w->bad && expression_tree_walker(n, gen_walker, w);
}

static Node *
gen_sub_mutator(Node *n, GenWalk *w)
{
	int			id;

	if (n == NULL)
		return NULL;
	if ((IsA(n, Var) || IsA(n, CoerceViaIO) || IsA(n, FuncExpr) || IsA(n, OpExpr) || IsA(n, RelabelType)) &&
		(id = match_leaf(w->c, n)) >= 0)
	{
		Param	   *p = makeNode(Param);
		int			pos = 0;

		while (w->leaf[pos] != id)
			pos++;
		p->paramkind = PARAM_EXEC;
		p->paramid = pos;
		p->paramtype = w->ptype[pos];
		p->paramtypmod = w->ptypmod[pos];
		p->paramcollid = w->pcoll[pos];
		p->location = -1;
		return (Node *) p;
	}
	return expression_tree_mutator(n, gen_sub_mutator, w);
}

/* does the expression read a column of a joined table */
bool
arg_joined(AggCtx *c, Node *arg)
{
	foreach_ptr(JoinCtx, jc, c->joins)
		if (bms_is_member(jc->rti, pull_varnos(c->root, arg)))
			return true;
	return false;
}

/* an argument of a generic aggregate: a leaf, an expression of up to SIFT_AGG_MAX_GLEAVES leaves, a constant, or an expression of a joined table's columns */
static bool
compile_gen_arg(AggCtx *c, Node *arg, SiftAggGenArg *out, double *cost, bool *mapped, int ai)
{
	GenWalk		w = {c};
	int			leaf;
	QualCost	qc;

	arg = normalize_expr(c, arg);
	out->typid = exprType(arg);
	if (arg_joined(c, arg))
	{
		TypeCacheEntry *tc = lookup_type_cache(out->typid, TYPECACHE_EQ_OPR);
		RegProcedure lhs,
					rhs;

		/* the value of each joined row is the combo's: one table of values per aggregate */
		if (ai < 0 || *mapped || !OidIsValid(tc->eq_opr) || !get_op_hash_functions(tc->eq_opr, &lhs, &rhs) ||
			!combo_slot(c, arg, true, &out->combo, &out->slot))
			return false;
		out->mapped = *mapped = true;
		*cost = 4.0;
		return true;
	}
	if ((leaf = match_leaf(c, strip_relabel(arg))) >= 0)
	{
		if (!gen_leaf_ok(c, leaf))
			return false;
		out->nleaves = 1;
		out->leaf[0] = leaf;
		use_leaf(c, leaf);
		*cost = 2.0;
		return true;
	}
	if (contain_volatile_functions(arg))
		return false;
	gen_walker(arg, &w);
	if (w.bad || (w.nl == 0 && !pseudo_const(arg)))
		return false;
	out->nleaves = w.nl;
	for (int p = 0; p < w.nl; p++)
	{
		out->leaf[p] = w.leaf[p];
		use_matched_leaf(c, w.leaf[p]);
	}
	out->expr = gen_sub_mutator(arg, &w);
	cost_qual_eval_node(&qc, arg, c->root);
	*cost = 2.0 + (w.nl ? qc.per_tuple / cpu_operator_cost : 0.0) + 2.0 * w.nl;
	return true;
}

/* the function's own cost in operator costs (its procost, or the planner support function's estimate) */
static double
fn_cost(AggCtx *c, Oid fn)
{
	QualCost	qc = {0, 0};

	add_function_cost(c->root, fn, NULL, &qc);
	return qc.per_tuple / cpu_operator_cost;
}

/*
 * Any aggregate with a transition function (and final function) that is not volatile runs on PostgreSQL's own functions: the
 * executor makes the aggregate's states as ExecAgg does (a minimal AggState for AggCheckCallContext and AggGetAggref). The
 * order of the input is the scan's (heap order of a fresh index), which is what the normal plan's sequential scan gives; an
 * ORDER BY in the call, a variadic aggregate, a hypothetical-set aggregate and an aggregate of another table's column keep the plan.
 */
/* a partial state per worker, merged by the combine function: the functions are parallel safe, the state is a plain type or comes with its serial and deserial functions */
static bool
gen_parallel_ok(Oid agg, Oid transfn, Oid combfn, Oid serfn, Oid deserfn, Oid transtype)
{
	if (!OidIsValid(combfn) || IsPolymorphicType(transtype) || func_volatile(combfn) == PROVOLATILE_VOLATILE)
		return false;
	if (transtype == INTERNALOID && (!OidIsValid(serfn) || !OidIsValid(deserfn)))
		return false;
	for (int i = 0; i < 5; i++)
	{
		Oid			fn = i == 0 ? agg : i == 1 ? transfn : i == 2 ? combfn : i == 3 ? serfn : deserfn;

		if (OidIsValid(fn) && func_parallel(fn) != PROPARALLEL_SAFE)
			return false;
	}
	return true;
}

/* the functions of the aggregate that decide how its state is carried and combined */
typedef struct GenFns
{
	Oid			transfn,
				combfn,
				serfn,
				deserfn,
				transtype;
} GenFns;

/* the shapes of aggregate call the generic path runs: plain, with ORDER BY or DISTINCT, ordered-set and hypothetical-set */
static bool
gen_eligible(Aggref *a, bool ordered, bool hyp)
{
	if ((a->aggkind != AGGKIND_NORMAL && !ordered) || a->aggsplit != AGGSPLIT_SIMPLE || list_length(a->args) > SIFT_AGG_MAX_GARGS ||
		(ordered && !hyp && (list_length(a->aggorder) != 1 || list_length(a->args) != 1)) ||
		(hyp && list_length(a->aggorder) != list_length(a->args)) ||
		(a->aggdistinct != NIL && ordered))
		return false;
	foreach_ptr(Node, d, a->aggdirectargs)
		if (!pseudo_const(d))
			return false;
	return a->aggdistinct == NIL || list_length(a->aggdistinct) == list_length(a->args);
}

/* the functions of aggregate agg; false when its transition or final function is volatile */
static bool
gen_fns(Oid agg, GenFns *f)
{
	HeapTuple	tup = SearchSysCache1(AGGFNOID, ObjectIdGetDatum(agg));
	Form_pg_aggregate ag;
	bool		volatile_fn;

	if (!HeapTupleIsValid(tup))
		elog(ERROR, "cache lookup failed for aggregate %u", agg);
	ag = (Form_pg_aggregate) GETSTRUCT(tup);
	volatile_fn = func_volatile(ag->aggtransfn) == PROVOLATILE_VOLATILE ||
		(OidIsValid(ag->aggfinalfn) && func_volatile(ag->aggfinalfn) == PROVOLATILE_VOLATILE);
	*f = (GenFns) {ag->aggtransfn, ag->aggcombinefn, ag->aggserialfn, ag->aggdeserialfn, ag->aggtranstype};
	ReleaseSysCache(tup);
	return !volatile_fn;
}

/* the text of the aggregate in EXPLAIN: txt is its arguments */
static char *
gen_desc(AggCtx *c, Aggref *a, SiftAggGen *g, const char *txt)
{
	StringInfoData s;

	initStringInfo(&s);
	if (g->ordered)
	{
		foreach_ptr(Node, e, a->aggdirectargs)
			appendStringInfo(&s, "%s%s", foreach_current_index(e) ? ", " : "", expr_text(c, e));
		return psprintf("%s(%s) WITHIN GROUP (ORDER BY %s)", get_func_name(a->aggfnoid), s.data, txt);
	}
	foreach_node(SortGroupClause, sc, a->aggorder)
	{
		bool		rev;
		TargetEntry *te = get_sortgroupref_tle(sc->tleSortGroupRef, a->args);

		get_equality_op_for_ordering_op(sc->sortop, &rev);
		appendStringInfo(&s, "%s%s%s%s", s.len ? ", " : " ORDER BY ", expr_text(c, (Node *) te->expr), rev ? " DESC" : "",
						 sc->nulls_first == rev ? "" : sc->nulls_first ? " NULLS FIRST" : " NULLS LAST");
	}
	return psprintf("%s(%s%s%s)", get_func_name(a->aggfnoid), g->distinct ? "DISTINCT " : "", txt, s.data);
}

bool
compile_gen(AggCtx *c, Aggref *a, SiftAggAgg *out, int ai)
{
	bool		hyp = a->aggkind == AGGKIND_HYPOTHETICAL,
				ordered = a->aggkind == AGGKIND_ORDERED_SET || hyp,
				hash = false;
	GenFns		fns;
	SiftAggGen *g;
	double		cost = 0;
	bool		mapped = false,
				wide = false;
	StringInfoData txt;

	if (!gen_eligible(a, ordered, hyp) || !gen_fns(a->aggfnoid, &fns))
		return false;
	g = palloc0(sizeof(SiftAggGen));
	initStringInfo(&txt);
	foreach_node(TargetEntry, te, a->args)
	{
		double		k;

		if (!compile_gen_arg(c, (Node *) te->expr, &g->arg[g->nargs], &k, &mapped, ai))
			return false;
		cost += k;
		if (!te->resjunk)
		{
			appendStringInfo(&txt, "%s%s", g->ntrans ? ", " : "", expr_text(c, (Node *) te->expr));
			g->ntrans++;
		}
		g->nargs++;
	}
	if (a->aggdistinct != NIL)
	{
		/* the distinct values of the one argument that varies (string_agg(DISTINCT x, ',')) are a hash set; any other DISTINCT is sorted */
		int			nvar = 0;

		for (int k = 0; k < g->nargs; k++)
			if (g->arg[k].nleaves > 0 || g->arg[k].mapped)
			{
				g->darg = k;
				nvar++;
			}
		hash = nvar == 1 && a->aggorder == NIL && !mapped;
	}
	if (hash || (ordered && !hyp))
	{
		Oid			t = g->arg[g->darg].typid;
		TypeCacheEntry *tc = lookup_type_cache(t, TYPECACHE_CMP_PROC_FINFO | TYPECACHE_EQ_OPR_FINFO);

		if (!OidIsValid(tc->cmp_proc) || !OidIsValid(tc->eq_opr))
			return false;
		if (ordered && !OidIsValid(linitial_node(SortGroupClause, a->aggorder)->sortop))
			return false;
	}
	for (int k = 0; k < g->nargs; k++)
		if (g->arg[k].mapped)
			slot_req(c, g->arg[k].combo, g->arg[k].slot)->agg = ai;
	g->aggref = (Node *) copyObject(a);
	((Aggref *) g->aggref)->aggfilter = NULL;
	g->direct = (Node *) a->aggdirectargs;
	g->distinct = a->aggdistinct != NIL;
	g->ordered = ordered;
	g->hyp = hyp;
	/* the representation of equal values (1.0, 1.00) that min and max keep follows the order of the rows, which the workers' do not */
	for (int k = 0; k < g->nargs; k++)
		for (int j = 0; j < g->arg[k].nleaves; j++)
			wide |= leaf_of(c, g->arg[k].leaf[j])->wide;
	g->parallel = !wide && !ordered && !mapped && !g->distinct && a->aggorder == NIL && gen_parallel_ok(a->aggfnoid, fns.transfn, fns.combfn, fns.serfn, fns.deserfn, fns.transtype);
	g->sorted = !ordered && !hash && (a->aggorder != NIL || a->aggdistinct != NIL);
	g->cost = AGG_GEN_ROW + fn_cost(c, fns.transfn) + cost + (g->distinct ? AGG_GEN_DISTINCT : 0) + (ordered ? AGG_GEN_ORDERED : 0) +
		(g->sorted ? AGG_GEN_SORTED : 0) + (hyp ? AGG_GEN_HYP : 0);
	out->kind = SAA_GEN;
	out->gen = g;
	out->restype = a->aggtype;
	out->collid = a->inputcollid;
	out->desc = gen_desc(c, a, g, txt.data);
	record_plan_function_dependency(c->root, a->aggfnoid);
	foreach_node(TargetEntry, te, a->args)
		record_deps(c->root, (Node *) te->expr);
	return true;
}
