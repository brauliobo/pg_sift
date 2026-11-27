/*
 * agg/plan/hybrid.c - what a scan that brings its own rows asks of the planner of SiftAggV6: the aggregate compiled without its quals (agg/hybrid/plan.c), sift.score(ctid) as the argument of an
 * aggregate, and the text query of the statement (body @@> 'q') read from the quals for the intake of its hits as bridges (v6_text.c).
 */
#include "postgres.h"

#include "access/genam.h"
#include "catalog/pg_type_d.h"
#include "nodes/nodeFuncs.h"
#include "parser/parsetree.h"

#include "agg/plan/int.h"
#include "agg/source_rel.h"
#include "agg/unnest/jsonb.h"
#include "bm25/planner/planner.h"

bool
sift_v6_score_col(V6Plan *p, const Expr *e, SiftV6Col *out)
{
	bool		is_max;

	if (!p->score_ok || e == NULL || !sift_is_score_expr((Node *) e, p->rel->relid, &is_max) || is_max)
		return false;
	*out = (SiftV6Col) {.attno = SIFT_V6_SCORE_ATTNO,.typid = FLOAT4OID};
	return true;
}

static bool
compile_once(V6Plan *p, IndexOptInfo *ix, Relation irel, const SiftStoreSchema *table, void *extra, List *group_exprs)
{
	if (table != NULL)
	{
		p->relid = planner_rt_fetch(p->rel->relid, p->root)->relid;
		p->ops = sift_store_ops_for_oid(p->relid, true);
		p->is_table = true;
		p->sch = *table;
	}
	else
	{
		p->relid = ix->indexoid;
		p->index = ix;
		sift_v6_plan_index(p, irel);
		if (p->ops == NULL)
			return false;
	}
	return sift_v6_compile_head(p, (GroupPathExtraData *) extra, group_exprs);
}

bool
sift_v6_members_dropped(const V6Plan *p)
{
	return p->spec.vqual != NIL || p->spec.nfilters > 0 || p->spec.ext != NULL || p->text != NULL;
}

/* the members of a map are read from the leaves of the index; a plan that drops rows after the source reads them from the document, as PostgreSQL does for the rows it reads */
void
sift_v6_source_group_path(PlannerInfo *root, RelOptInfo *input_rel, RelOptInfo *output_rel, const SiftV6RelSource *s, GroupPathExtraData *extra, List *group_exprs)
{
	Relation	rel = sift_v6_source_open(s->reloid, s->is_table, AccessShareLock, NULL);

	for (bool members = true;; members = false)
	{
		V6Plan		p = {.root = root,.rel = input_rel,.out = output_rel,.batch_ok = true,.members = members};
		bool		ok;

		if (!sift_v6_plan_bind(&p, s, rel) || (!s->is_table && sift_v6_mapkey_paths(&p, extra, group_exprs)))
			break;
		ok = sift_v6_compile_on(&p, extra, group_exprs);
		if (members && p.spec.jmembers != NIL && (!ok || sift_v6_members_dropped(&p)))
			continue;
		if (ok)
			sift_v6_add_group_path(&p, output_rel, group_exprs, extra != NULL ? (List *) extra->havingQual : NIL);
		break;
	}
	sift_v6_source_close(rel, s->is_table, NoLock);
}

bool
sift_v6_compile_aggregate(PlannerInfo *root, RelOptInfo *rel, RelOptInfo *out, IndexOptInfo *ix, Relation irel, const SiftStoreSchema *table, void *extra, SiftV6Spec *spec, List **scan_tlist,
						  List **group_exprs)
{
	V6Plan		p = {.root = root,.rel = rel,.out = out,.score_ok = true,.members = true};

	*group_exprs = sift_v6_group_exprs_of(root);
	if (!compile_once(&p, ix, irel, table, extra, *group_exprs))
		return false;
	if (p.spec.jmembers != NIL && sift_v6_members_dropped(&p))
	{
		p = (V6Plan) {.root = root,.rel = rel,.out = out,.score_ok = true};
		if (!compile_once(&p, ix, irel, table, extra, *group_exprs))
			return false;
	}
	*spec = p.spec;
	*scan_tlist = p.scan_tlist;
	return true;
}

/*
 * A text query of a base relation: its hits keep the rows (a partition of a partitioned table has its own text index, so no partition takes this). Each clause is an OR of
 * searches of one index and the clauses are ANDed, so the arms of the search are the product of their branches, an arm the AND of a branch of each
 */
bool
sift_v6_text_qual(V6Plan *p, RestrictInfo *ri)
{
	List	   *branches;
	List	   *arms = NIL;
	IndexOptInfo *ix;

	if (p->rel->reloptkind != RELOPT_BASEREL || (ix = sift_v6_text_match(p->rel, ri, &branches)) == NULL || (p->text != NULL && ix != p->text) ||
		Max(list_length(p->text_queries), 1) * list_length(branches) > SIFT_V6_TEXT_MAX_ARMS)
		return false;
	foreach_node(List, arm, p->text != NULL ? p->text_queries : list_make1(NIL))
		foreach_ptr(Expr, q, branches)
			arms = lappend(arms, lappend(list_copy(arm), q));
	p->text = ix;
	p->text_queries = arms;
	p->text_quals = lappend(p->text_quals, ri);
	return true;
}

bool
sift_v6_compile_text(V6Plan *p)
{
	if (p->text == NULL)
		return true;
	if (p->xseed != NULL && p->xseed->unnest)
		return false;
	p->spec.text_indexoid = p->text->indexoid;
	p->spec.text_queries = p->text_queries;
	return sift_v6_text_usable(p->text, p->text_queries);
}
