/*
 * agg/plan/placeholder.c - the placeholders of a statement (an expression of a sub-select PostgreSQL pulls up into a join, which an outer join above it may null) as the scan computes them.
 */
#include "postgres.h"

#include "catalog/pg_type_d.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "optimizer/placeholder.h"
#include "parser/parsetree.h"
#include "rewrite/rewriteManip.h"
#include "utils/lsyscache.h"

#include "agg/ext.h"
#include "agg/join/pairs.h"
#include "agg/plan/int.h"
#include "agg/unnest/jsonb.h"

/* the outer join oj is the LEFT JOIN of an unnest: the scan reads whether a pair has its element */
static bool
elem_left_join(PlannerInfo *root, Index oj)
{
	foreach_node(SpecialJoinInfo, sj, root->join_info_list)
		if (sj->ojrelid == oj && sj->jointype == JOIN_LEFT)
		{
			int			r = -1;

			while ((r = bms_next_member(sj->min_righthand, r)) >= 0)
				if (planner_rt_fetch(r, root)->rtekind == RTE_FUNCTION)
					return true;
		}
	return false;
}

/* the outer joins that null a placeholder: not the grouping of 18, whose grouping sets null their keys in the rows of a super-aggregate, as the scan computes them */
static Relids
outer_nulling(PlannerInfo *root, const PlaceHolderVar *phv)
{
#if PG_VERSION_NUM >= 180000
	return bms_del_member(bms_copy(phv->phnullingrels), root->group_rtindex);
#else
	return phv->phnullingrels;
#endif
}

bool
sift_v6_nulled_placeholder(Node *n, PlannerInfo *root)
{
	Relids		oj;

	if (n == NULL)
		return false;
	if (IsA(n, PlaceHolderVar) && !bms_is_empty(oj = outer_nulling(root, (const PlaceHolderVar *) n)) && !sift_v6_placeholder_is_expr(root, (PlaceHolderVar *) n) &&
		(bms_membership(oj) != BMS_SINGLETON || !elem_left_join(root, bms_singleton_member(oj))))
		return true;
	return expression_tree_walker(n, sift_v6_nulled_placeholder, root);
}

/* the outer join of the statement that nulls the element of the unnest of p for a row with none, whose presence the scan reads; 0: none */
static Index
elem_join(const V6Plan *p)
{
	if (p->elem_rel == NULL || p->xseed == NULL || !sift_v6_ext_stream(p->xseed, p->elem_stream)->left)
		return 0;
	foreach_node(SpecialJoinInfo, sj, p->root->join_info_list)
		if (sj->jointype == JOIN_LEFT && bms_is_member(p->elem_rel->relid, sj->min_righthand))
			return sj->ojrelid;
	return 0;
}

typedef struct Contents
{
	V6Plan	   *p;
	Index		oj;
	bool		ok;
} Contents;

/* a column of the fact table that no row has NULL, as the outer join oj nulls it when it keeps a row of its other side alone; NULL when oj does not null the fact table or the scan reads none */
static Expr *
fact_present(V6Plan *p, Index oj)
{
	const RelOptInfo *rel = p->rel;
	SiftV6Col	col;
	int			att = 0;

	foreach_node(SpecialJoinInfo, sj, p->root->join_info_list)
		if (sj->ojrelid == oj && (bms_is_member(rel->relid, sj->min_righthand) || (sj->jointype == JOIN_FULL && bms_is_member(rel->relid, sj->min_lefthand))))
			while ((att = bms_next_member(rel->notnullattnums, att)) > 0)
			{
				Var		   *v = makeVar(rel->relid, (AttrNumber) att, InvalidOid, -1, InvalidOid, 0);

				get_atttypetypmodcoll(planner_rt_fetch(rel->relid, p->root)->relid, v->varattno, &v->vartype, &v->vartypmod, &v->varcollid);
				v->varnullingrels = bms_make_singleton(oj);
				if (sift_v6_read_of(p, (Expr *) v, true, &col))
					return (Expr *) v;
			}
	return NULL;
}

/* true for a row the outer join oj kept with its nullable side: the element of the unnest is there, or the row of the fact table; NULL when the scan cannot tell */
static Node *
kept_by(Contents *c, Index oj)
{
	NullTest   *t = makeNode(NullTest);

	t->arg = oj == c->oj ? (Expr *) makeVar(c->p->elem_rel->relid, SIFT_V6_ELEM_PRESENT, INT8OID, -1, InvalidOid, 0) : fact_present(c->p, oj);
	t->nulltesttype = IS_NOT_NULL;
	t->location = -1;
	return t->arg != NULL ? (Node *) t : NULL;
}

static Node *
contents_mutator(Node *n, Contents *c)
{
	const PlaceHolderVar *phv = (const PlaceHolderVar *) n;
	Relids		ojs;
	List	   *kept = NIL;
	Node	   *e;
	int			oj = -1;

	if (n == NULL)
		return NULL;
	if (!IsA(n, PlaceHolderVar))
		return expression_tree_mutator(n, contents_mutator, c);
	e = contents_mutator((Node *) phv->phexpr, c);
	if (bms_is_empty(ojs = outer_nulling(c->p->root, phv)))
		return e;
	if (sift_v6_placeholder_is_expr(c->p->root, (PlaceHolderVar *) phv))
		return add_nulling_relids(e, find_placeholder_info(c->p->root, (PlaceHolderVar *) phv)->ph_eval_at, ojs);
	while ((oj = bms_next_member(ojs, oj)) >= 0)
	{
		Node	   *k = kept_by(c, (Index) oj);

		c->ok &= k != NULL;
		kept = lappend(kept, k);
	}
	return c->ok ? sift_v6_case_when((Node *) make_ands_explicit(kept), e) : e;
}

bool
sift_v6_placeholder_values(V6Plan *p, Node *n, Node **out)
{
	Contents	c = {p, elem_join(p), true};

	*out = contents_mutator(n, &c);
	return c.ok;
}

Node *
sift_v6_case_when(Node *cond, Node *e)
{
	CaseExpr   *c = makeNode(CaseExpr);
	CaseWhen   *w = makeNode(CaseWhen);

	w->expr = (Expr *) cond;
	w->result = (Expr *) e;
	w->location = -1;
	c->casetype = exprType(e);
	c->casecollid = exprCollation(e);
	c->args = list_make1(w);
	c->defresult = (Expr *) makeNullConst(c->casetype, exprTypmod(e), c->casecollid);
	c->location = -1;
	return (Node *) c;
}
