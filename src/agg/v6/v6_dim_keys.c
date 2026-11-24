/*
 * agg/v6/v6_dim_keys.c - the plan of a dimension that probes its index by the constant keys of the fact table's filter (agg/v6/v6_dim_keys.h).
 */
#include "postgres.h"

#include "nodes/makefuncs.h"
#include "optimizer/cost.h"
#include "optimizer/pathnode.h"
#include "optimizer/paths.h"
#include "optimizer/restrictinfo.h"
#include "parser/parsetree.h"
#include "utils/array.h"
#include "utils/selfuncs.h"
#include "utils/typcache.h"

#include "agg/v6/v6_dim_keys.h"

/* fact.col = ANY (constant array) of the fact table's filter, col being the column attno: the equality of the type, so that what it keeps of the column is what a join on the column keeps */
static const ScalarArrayOpExpr *
fact_key_list(const RelOptInfo *fact, AttrNumber attno)
{
	foreach_node(RestrictInfo, ri, fact->baserestrictinfo)
	{
		const ScalarArrayOpExpr *sa = (const ScalarArrayOpExpr *) ri->clause;
		const Var  *v;
		const Const *c;

		if (!IsA(ri->clause, ScalarArrayOpExpr) || !sa->useOr || list_length(sa->args) != 2)
			continue;
		v = (const Var *) linitial(sa->args);
		c = (const Const *) lsecond(sa->args);
		if (IsA(v, Var) && IsA(c, Const) && !c->constisnull && v->varno == fact->relid && v->varattno == attno && v->varlevelsup == 0 &&
			sa->opno == lookup_type_cache(v->vartype, TYPECACHE_EQ_OPR)->eq_opr && sa->inputcollid == v->varcollid)
			return sa;
	}
	return NULL;
}

/* a probe a key reads rows scattered over the pages of the table: a list of more than half of the distinct keys reads it whole through the index, and plans the index for nothing */
#define KEYED_MAX_FRACTION	0.5

static bool
list_is_selective(PlannerInfo *root, const Var *key, const ScalarArrayOpExpr *sa)
{
	const ArrayType *keys = DatumGetArrayTypeP(((const Const *) lsecond(sa->args))->constvalue);
	VariableStatData vardata;
	bool		isdefault;
	double		ndistinct;

	examine_variable(root, (Node *) key, 0, &vardata);
	ndistinct = get_variable_numdistinct(&vardata, &isdefault);
	ReleaseVariableStats(vardata);
	return !isdefault && ArrayGetNItems(ARR_NDIM(keys), ARR_DIMS(keys)) <= ndistinct * KEYED_MAX_FRACTION;
}

/* key = ANY (the array of sa) as a condition of the dimension, when the key is of the column's type */
static RestrictInfo *
key_condition(PlannerInfo *root, const RelOptInfo *fact, const ScalarArrayOpExpr *sa, const Var *key)
{
	ScalarArrayOpExpr *cond = copyObject((ScalarArrayOpExpr *) sa);
	const Var  *col = (const Var *) linitial(sa->args);

	if (key->vartype != col->vartype || key->varcollid != col->varcollid || !list_is_selective(root, key, sa))
		return NULL;
	linitial(cond->args) = copyObject((Var *) key);
	return make_simple_restrictinfo(root, (Expr *) cond);
}

/* the relation as the planner left it, but for the conditions: its own plans are made again with them, and its indexes belong to it */
static RelOptInfo *
restricted_rel(PlannerInfo *root, const RelOptInfo *dim, List *conds)
{
	RelOptInfo *rel = makeNode(RelOptInfo);

	*rel = *dim;
	rel->pathlist = rel->ppilist = rel->partial_pathlist = rel->cheapest_parameterized_paths = NIL;
	rel->cheapest_startup_path = rel->cheapest_total_path = NULL;
	rel->baserestrictinfo = list_concat(list_copy(dim->baserestrictinfo), conds);
	rel->indexlist = NIL;
	foreach_ptr(IndexOptInfo, ix, dim->indexlist)
	{
		IndexOptInfo *own = makeNode(IndexOptInfo);

		*own = *ix;
		own->rel = rel;
		rel->indexlist = lappend(rel->indexlist, own);
	}
	set_baserel_size_estimates(root, rel);
	check_index_predicates(root, rel);
	return rel;
}

Path *
sift_v6_dim_path(PlannerInfo *root, const RelOptInfo *fact, RelOptInfo *dim, SiftV6JoinMode mode, int nkeys, const AttrNumber *probe_attno, const Var *const *key)
{
	Path	   *best = dim->cheapest_total_path;
	List	   *conds = NIL;
	RelOptInfo *rel;

	if (dim->reloptkind != RELOPT_BASEREL || dim->indexlist == NIL || planner_rt_fetch(dim->relid, root)->rtekind != RTE_RELATION ||
		(mode != SV6J_INNER_EQUI && mode != SV6J_LEFT_EQUI && mode != SV6J_SEMI_EQUI && mode != SV6J_ANTI_EQUI))
		return best;
	for (int k = 0; k < nkeys; k++)
	{
		const ScalarArrayOpExpr *sa = probe_attno[k] == 0 ? NULL : fact_key_list(fact, probe_attno[k]);
		RestrictInfo *cond = sa == NULL ? NULL : key_condition(root, fact, sa, key[k]);

		if (cond != NULL)
			conds = lappend(conds, cond);
	}
	if (conds == NIL)
		return best;
	rel = restricted_rel(root, dim, conds);
	create_index_paths(root, rel);
	foreach_ptr(Path, p, rel->pathlist)
		if (p->param_info == NULL && p->total_cost < best->total_cost)
			best = p;
	return best;
}
