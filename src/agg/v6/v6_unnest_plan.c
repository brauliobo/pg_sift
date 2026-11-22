/*
 * agg/v6/v6_unnest_plan.c - CROSS JOIN LATERAL unnest(col) AS u(elem) (or LEFT JOIN LATERAL ... ON true) over an array column of the fact table as part of the aggregate scan of SiftAggV6: when the paths of
 * the index keep the array of the column as a list (col[] text, ids[] int4: a leaf of its lengths and a leaf of its elements), the node takes the elements from the index and the keys and
 * aggregates of the statement may name the element beside the columns of the table (agg/v6/v6_unnest_run.h). The WHERE clauses of the table are evaluated on the batches, which have no scan keys.
 *
 * The unnested relation takes no qual of its own (a condition on the element is not the node's), and the statement reads no other relation.
 */
#include "postgres.h"

#include "nodes/nodeFuncs.h"
#include "optimizer/optimizer.h"
#include "parser/parsetree.h"
#include "utils/fmgroids.h"
#include "utils/lsyscache.h"

#include "agg/plan/format_ops.h"
#include "agg/v6/v6_bytes.h"
#include "agg/v6/v6_filter_pg.h"
#include "agg/v6/plan_int.h"
#include "agg/v6/v6_ext.h"
#include "col/am/format_ops.h"

/* the leaves of the array of attribute att: its lengths and its elements */
static bool
array_leaves(const SiftColRel *r, int att, Oid elem, int *group, int *leaf)
{
	*group = *leaf = -1;
	for (int i = r->first[att]; i < r->first[att] + r->nleaves[att]; i++)
		if (r->schema->cols[i].kind == SIFT_COLK_GROUP)
			*group = i;
	for (int i = r->first[att]; *group >= 0 && i < r->first[att] + r->nleaves[att]; i++)
	{
		const SiftColDef *d = &r->schema->cols[i];

		if (d->parent == *group && d->kind == SIFT_COLK_SCALAR && d->lane == 0 && !(d->flags & SIFT_COLD_LOSSY) &&
			(sift_v6_class(elem) == SV6C_TEXT ? d->store == SIFT_COLS_STR : d->store == SIFT_COLS_INT))
			*leaf = i;
	}
	return *leaf >= 0;
}

/* the unnest of the array column of a table in the FROM clause: its element type, and the Var of the column */
static bool
unnest_of(PlannerInfo *root, const RelOptInfo *rel, Index fact, const Var **array, Oid *elem)
{
	const RangeTblEntry *rte = planner_rt_fetch(rel->relid, root);
	const RangeTblFunction *rtf;
	const FuncExpr *fe;
	Oid			e;

	if (rel->reloptkind != RELOPT_BASEREL || rte->rtekind != RTE_FUNCTION || rte->funcordinality || list_length(rte->functions) != 1 || rel->baserestrictinfo != NIL ||
		rel->joininfo != NIL || !bms_equal(rel->lateral_relids, bms_make_singleton(fact)))
		return false;
	rtf = linitial(rte->functions);
	fe = (const FuncExpr *) rtf->funcexpr;
	if (!IsA(rtf->funcexpr, FuncExpr) || rtf->funccolcount != 1 || list_length(fe->args) != 1 || fe->funcid != F_UNNEST_ANYARRAY || !IsA(linitial(fe->args), Var))
		return false;
	*array = linitial(fe->args);
	e = get_element_type((*array)->vartype);
	*elem = e;
	return (*array)->varno == fact && (*array)->varlevelsup == 0 && fe->funcresulttype == e && (e == TEXTOID || e == VARCHAROID || e == INT2OID || e == INT4OID || e == INT8OID);
}

/* the join is inner or a left one of the unnest with no condition; any other special join is not ours */
static bool
join_kind(PlannerInfo *root, Index elem_rti, bool *left)
{
	*left = false;
	foreach_node(SpecialJoinInfo, sj, root->join_info_list)
	{
		if (sj->jointype != JOIN_LEFT || !bms_equal(sj->min_righthand, bms_make_singleton(elem_rti)) || *left)
			return false;
		*left = true;
	}
	return true;
}

static bool
seed_unnest(V6Plan *p, RelOptInfo *elem_rel, const Var *array, Oid elem, bool left)
{
	SiftV6Ext  *x = sift_v6_ext_new();
	int			group,
				leaf;

	for (int i = 0; i < p->index->ncolumns; i++)
		if (p->index->indexkeys[i] == array->varattno)
		{
			if (!array_leaves(p->colrel, i + 1, elem, &group, &leaf))
				return false;
			x->unnest = true;
			x->elem = (SiftV6XElem) {.attno = i + 1,.type = elem,.group_leaf = group,.elem_leaf = leaf,.left = left};
			p->xseed = x;
			p->elem_rel = elem_rel;
			return true;
		}
	return false;
}

/* a key of a jsonb document is read from the batches of the scan: the stream of the elements does not carry the payloads of its rows that the leaf cannot hold */
static bool
reads_json(const SiftV6Spec *s)
{
	for (int i = 0; i < s->nkeys; i++)
		if (s->key[i].jleaf > 0)
			return true;
	for (int j = 0; j < s->naggs; j++)
		if (s->agg[j].arg.jleaf > 0 || s->agg[j].arg2.jleaf > 0)
			return true;
	return false;
}

static bool
where_clause(V6Plan *p)
{
	SiftClause *clause;
	SiftBytes	canon;

	if (p->spec.indexqual == NIL)
		return true;
	clause = palloc(sizeof(SiftClause));
	sift_bytes_init(&canon, CurrentMemoryContext);
	if (!sift_v6_clause_make(CurrentMemoryContext, p->spec.indexqual, &p->spec, clause, &canon))
		return false;
	p->spec.ext->where = p->spec.indexqual;
	p->spec.indexqual = NIL;
	return true;
}

static void
try_fact(PlannerInfo *root, RelOptInfo *input_rel, RelOptInfo *output_rel, RelOptInfo *fact, RelOptInfo *elem_rel, GroupPathExtraData *extra, List *group_exprs)
{
	const Var  *array;
	Oid			elem;
	bool		left;

	if (!sift_v6_plan_fact_ok(root, fact) || !unnest_of(root, elem_rel, fact->relid, &array, &elem) || !join_kind(root, elem_rel->relid, &left))
		return;
	foreach_ptr(IndexOptInfo, ix, fact->indexlist)
	{
		V6Plan		p = {.root = root,.rel = fact,.out = output_rel,.relid = ix->indexoid,.index = ix};
		Relation	irel;

		if (ix->indexprs != NIL || ix->hypothetical || !sift_agg_format_ops_owns(sift_agg_format_ops(sift_col_v2_ops.format_major), ix->indexoid))
			continue;
		irel = index_open(ix->indexoid, NoLock);
		p.colrel = sift_col_rel(irel);
		p.f2 = p.colrel->stale ? NULL : sift_f2_rel(irel);
		if (p.f2 != NULL && seed_unnest(&p, elem_rel, array, elem, left) && sift_v6_compile_on(&p, extra, group_exprs) && p.spec.nbins == 0 && p.spec.jsonqual == NIL && !reads_json(&p.spec) &&
			where_clause(&p))
			sift_v6_add_join_path(&p, group_exprs, (List *) extra->havingQual, NIL, 0, input_rel->rows * cpu_operator_cost * 4.0, input_rel->rows);
		index_close(irel, NoLock);
	}
}

void
sift_v6_unnest_upper_paths(PlannerInfo *root, RelOptInfo *input_rel, RelOptInfo *output_rel, void *extra)
{
	List	   *group_exprs = NIL;
	Relids		base = sift_v6_plan_base_relids(root, input_rel);
	int			a,
				b;

	if (!sift_v6_plan_query_ok(root, output_rel) || bms_num_members(base) != 2)
		return;
	a = bms_next_member(base, -1);
	b = bms_next_member(base, a);
	foreach_node(SortGroupClause, sgc, root->processed_groupClause)
		group_exprs = lappend(group_exprs, get_sortgroupclause_expr(sgc, root->parse->targetList));
	try_fact(root, input_rel, output_rel, root->simple_rel_array[a], root->simple_rel_array[b], (GroupPathExtraData *) extra, group_exprs);
	try_fact(root, input_rel, output_rel, root->simple_rel_array[b], root->simple_rel_array[a], (GroupPathExtraData *) extra, group_exprs);
}

bool
sift_v6_unnest_column(V6Plan *p, const Expr *e, SiftV6Col *out)
{
	const Var  *v = (const Var *) e;
	const SiftV6Class cls = IsA(e, Var) ? sift_v6_class(v->vartype) : SV6C_NONE;

	if (p->elem_rel == NULL || !IsA(e, Var) || v->varno != p->elem_rel->relid || v->varattno != 1 || v->varlevelsup != 0 || (cls != SV6C_I32 && cls != SV6C_I64 && cls != SV6C_TEXT))
		return false;
	*out = (SiftV6Col) {.attno = p->spec.ext->elem.attno,.typid = v->vartype,.src = SV6SRC_ELEM};
	return true;
}
