/*
 * agg/v6/v6_unnest_plan.c - CROSS JOIN LATERAL unnest(col) AS u(elem), jsonb_to_recordset(col) AS u(c1 t1, ..) (or LEFT JOIN LATERAL ... ON true) over an array column of the fact table as part of the
 * aggregate scan of SiftAggV6: when the paths of the index keep the array of the column as a list (col[] text, ids[] int4, items[]{sku text, price float8}: a leaf of its lengths and a leaf of its
 * elements or of each key of its objects), the node takes the elements from the index and the keys and aggregates of the statement may name the elements beside the columns of the table
 * (agg/v6/v6_unnest_run.h). The WHERE clauses of the table and of the elements are evaluated on the batches, which have no scan keys.
 *
 * The statement reads no other relation. The planning of the lateral relation itself is agg/v6/v6_unnest_jsonb.h.
 */
#include "postgres.h"

#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "optimizer/optimizer.h"
#include "parser/parsetree.h"
#include "utils/fmgroids.h"
#include "utils/lsyscache.h"

#include "agg/v6/v6_bytes.h"
#include "agg/v6/v6_filter_pg.h"
#include "agg/v6/plan_int.h"
#include "agg/v6/v6_ext.h"
#include "agg/v6/v6_unnest_jsonb.h"

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

/* what the batches of the scan compute is the month of a date column (the one binning of a key the scan of an unnest does), float8 arithmetic of arguments (agg/v6/v6_plan_rewrite.h) and the PostgreSQL
 * expressions of keys and arguments over the columns of the pairs (v6_pg_expr.h), and no other */
static bool
computes_only_months(const SiftV6Spec *s)
{
	for (int b = 0; b < s->nbins; b++)
		if (s->bin[b].stride_us != SIFT_DATEBIN_MONTH || s->key[s->bin[b].key].typid != DATEOID)
			return false;
	for (int k = 0; k < s->nderive; k++)
		if (s->derive[k].kind != SVD_ARITH && s->derive[k].kind != SVD_PG_EXPR)
			return false;
	for (int i = 0; i < s->nkeys; i++)
		if (s->key[i].derive != 0 && s->derive[s->key[i].derive - 1].kind != SVD_PG_EXPR)
			return false;
	return true;
}

bool
sift_v6_unnest_where(V6Plan *p)
{
	SiftClause *clause;
	SiftBytes	canon;

	foreach_node(RestrictInfo, ri, p->rel->baserestrictinfo)
		if (ri != p->sub_qual)
			p->spec.ext->heap_where = lappend(p->spec.ext->heap_where, copyObject(ri->clause));
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

/* the statement compiled on the index to what the scan of an unnest computes */
bool
sift_v6_unnest_compile(V6Plan *p, GroupPathExtraData *extra, List *group_exprs)
{
	return sift_v6_compile_on(p, extra, group_exprs) && computes_only_months(&p->spec) && p->spec.jsonqual == NIL;
}

/* what the unnest scan needs of the compiled statement besides its columns: the quals of the element and of the table run on the batches */
bool
sift_v6_unnest_ready(V6Plan *p)
{
	return computes_only_months(&p->spec) && p->spec.jsonqual == NIL && sift_v6_unnest_elem_quals(p) && sift_v6_unnest_where(p);
}

/* the pairs the filter of an aggregate drops are not skipped by an argument PostgreSQL computes for every pair: as the CASE of its filter, the executor leaves them out of the argument */
Aggref *
sift_v6_unnest_filter_as_case(const V6Plan *p, Aggref *a)
{
	TargetEntry *te;
	CaseWhen   *when;
	CaseExpr   *c;
	Aggref	   *plain;

	if (p->elem_rel == NULL || a->aggfilter == NULL || list_length(a->args) != 1 || a->aggdistinct != NIL || a->aggorder != NIL)
		return a;
	te = linitial_node(TargetEntry, a->args);
	if (IsA(te->expr, Var) || exprType((Node *) te->expr) != FLOAT8OID ||
		!bms_equal(pull_varnos(p->root, (Node *) list_make2(a->aggfilter, te->expr)), bms_make_singleton(p->elem_rel->relid)))
		return a;
	when = makeNode(CaseWhen);
	when->expr = a->aggfilter;
	when->result = te->expr;
	c = makeNode(CaseExpr);
	c->casetype = FLOAT8OID;
	c->args = list_make1(when);
	c->defresult = (Expr *) makeNullConst(FLOAT8OID, -1, InvalidOid);
	plain = copyObject(a);
	plain->aggfilter = NULL;
	linitial_node(TargetEntry, plain->args)->expr = (Expr *) c;
	return plain;
}

/* one operator per pair, and each group of a distinct count or a percentile keeps the values it sees (7 times a pair that folds into a counter, as measured on 3.7M pairs in 60,000 groups) */
#define HEAVY_AGG_PAIR_FACTOR	6

/* the pairs of the whole table: the batches of the scan come from every granule of the index (the pull prunes none by the quals of the table), so a selective statement costs what the table does */
Cost
sift_v6_unnest_pairs_cost(const V6Plan *p, const RelOptInfo *input_rel)
{
	const double sel = Max(clauselist_selectivity(p->root, p->rel->baserestrictinfo, 0, JOIN_INNER, NULL), 1.0e-6);
	int			heavy = 0;

	for (int j = 0; p->spec.nkeys > 0 && j < p->spec.naggs; j++)
		heavy += sift_v6_kind_distinct(p->spec.agg[j].kind) || p->spec.agg[j].kind == SV6_PCT_CONT;
	return input_rel->rows / sel * cpu_operator_cost * (1 + HEAVY_AGG_PAIR_FACTOR * heavy);
}

static void
try_fact(PlannerInfo *root, RelOptInfo *input_rel, RelOptInfo *output_rel, RelOptInfo *fact, RelOptInfo *elem_rel, GroupPathExtraData *extra, List *group_exprs)
{
	SiftV6ElemRte rte;
	bool		left;

	if (!sift_v6_plan_fact_ok(root, fact) || !sift_v6_unnest_rte(root, fact, elem_rel, &rte) || !join_kind(root, elem_rel->relid, &left))
		return;
	foreach_ptr(IndexOptInfo, ix, fact->indexlist)
	{
		V6Plan		p = {.root = root,.rel = fact,.out = output_rel,.relid = ix->indexoid,.index = ix};
		Relation	irel;

		if (!sift_v6_index_owned(ix))
			continue;
		irel = index_open(ix->indexoid, AccessShareLock);
		sift_v6_plan_index(&p, irel);
		if (p.ops != NULL && sift_v6_unnest_seed(&p, elem_rel, &rte, left) && sift_v6_compile_on(&p, extra, group_exprs) && sift_v6_unnest_ready(&p))
			sift_v6_add_join_path(&p, group_exprs, (List *) extra->havingQual, NIL, 0, sift_v6_unnest_pairs_cost(&p, input_rel), input_rel->rows);
		index_close(irel, NoLock);
	}
}

void
sift_v6_unnest_upper_paths(PlannerInfo *root, RelOptInfo *input_rel, RelOptInfo *output_rel, void *extra)
{
	List	   *group_exprs;
	Relids		base = sift_v6_plan_base_relids(root, input_rel);
	int			a,
				b;

	if (!sift_v6_plan_query_ok(root, output_rel) || bms_num_members(base) != 2)
		return;
	a = bms_next_member(base, -1);
	b = bms_next_member(base, a);
	group_exprs = sift_v6_group_exprs_of(root);
	try_fact(root, input_rel, output_rel, root->simple_rel_array[a], root->simple_rel_array[b], (GroupPathExtraData *) extra, group_exprs);
	try_fact(root, input_rel, output_rel, root->simple_rel_array[b], root->simple_rel_array[a], (GroupPathExtraData *) extra, group_exprs);
}
