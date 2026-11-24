/*
 * agg/v6/plan_hybrid.c - what a scan that brings its own rows asks of the planner of SiftAggV6: the aggregate compiled without its quals (agg/hybrid_plan.c), sift.score(ctid) as the argument of an
 * aggregate, and the text query of the statement (body @@> 'q') read from the quals for the intake of its hits as bridges (v6_text.c).
 */
#include "postgres.h"

#include "access/genam.h"
#include "catalog/pg_type_d.h"
#include "nodes/nodeFuncs.h"
#include "parser/parsetree.h"

#include "agg/v6/plan_int.h"
#include "bm25/planner.h"

bool
sift_v6_score_col(V6Plan *p, const Expr *e, SiftV6Col *out)
{
	bool		is_max;

	if (!p->score_ok || e == NULL || !sift_is_score_expr((Node *) e, p->rel->relid, &is_max) || is_max)
		return false;
	*out = (SiftV6Col) {.attno = SIFT_V6_SCORE_ATTNO,.typid = FLOAT4OID};
	return true;
}

bool
sift_v6_compile_aggregate(PlannerInfo *root, RelOptInfo *rel, RelOptInfo *out, IndexOptInfo *ix, Relation irel, const SiftStoreSchema *table, void *extra, SiftV6Spec *spec, List **scan_tlist,
						  List **group_exprs)
{
	V6Plan		p = {.root = root,.rel = rel,.out = out,.score_ok = true};

	*group_exprs = sift_v6_group_exprs_of(root);
	if (table != NULL)
	{
		p.relid = planner_rt_fetch(rel->relid, root)->relid;
		p.ops = sift_store_ops_for_scan(p.relid, true);
		p.is_table = true;
		p.tsch = *table;
	}
	else
	{
		p.relid = ix->indexoid;
		p.index = ix;
		sift_v6_plan_index(&p, irel);
		if (p.ops == NULL)
			return false;
	}
	if (!sift_v6_compile_head(&p, (GroupPathExtraData *) extra, *group_exprs))
		return false;
	*spec = p.spec;
	*scan_tlist = p.scan_tlist;
	return true;
}

/* The text query of a base relation: its hits keep the rows (a partition of a partitioned table has its own text index, so no partition takes this) */
bool
sift_v6_text_qual(V6Plan *p, RestrictInfo *ri)
{
	if (p->is_table || p->text != NULL || p->rel->reloptkind != RELOPT_BASEREL || (p->text = sift_v6_text_match(p->rel, ri, &p->text_query)) == NULL)
		return false;
	p->text_qual = ri;
	return true;
}

bool
sift_v6_compile_text(V6Plan *p)
{
	if (p->text == NULL)
		return true;
	p->spec.text_indexoid = p->text->indexoid;
	p->spec.text_query = p->text_query;
	return sift_v6_text_usable(p->text, p->text_query);
}
