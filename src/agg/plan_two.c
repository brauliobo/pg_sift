/*
 * agg/plan_two.c - two level aggregation: an outer aggregate over aggregates grouped by row
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

/* ---------- two level: an outer aggregate over aggregates grouped by row ---------- */

#define AGG_MAX_ARMS	4

typedef struct CteSub
{
	Index		rti;			/* 0: the arm reads the table itself */
	List	   *tlist;			/* the target list of the common table expression */
} CteSub;

static Node *
cte_sub_mutator(Node *n, CteSub *s)
{
	if (n == NULL)
		return NULL;
	if (s->rti != 0 && IsA(n, Var) && ((Var *) n)->varno == s->rti && ((Var *) n)->varlevelsup == 0)
	{
		TargetEntry *te = get_tle_by_resno(s->tlist, ((Var *) n)->varattno);

		return te ? (Node *) copyObject(te->expr) : n;
	}
	return expression_tree_mutator(n, cte_sub_mutator, s);
}

/* what an arm (a subquery of the outer query) is, in the terms of the table it reads */
typedef struct ArmIn
{
	CteSub		sub;
	List	   *cols;			/* the expressions of its output columns */
	List	   *group;			/* its group expressions */
	List	   *gsgc;			/* their SortGroupClauses */
	List	   *used;			/* the group expressions that are an outer key */
	List	   *quals;			/* its WHERE, when it reads a common table expression */
	Node	   *id;				/* the group expression that is unique per row */
	SiftAggArm *out;
} ArmIn;

/* the root of the common table expression a CTE scan of `root` reads; NULL if it is not one the scan can look through */
static PlannerInfo *
cte_root(PlannerInfo *root, RangeTblEntry *rte)
{
	PlannerInfo *cteroot = root;
	int			ndx = 0;
	CommonTableExpr *cte = NULL;

	for (Index up = rte->ctelevelsup; up > 0 && cteroot != NULL; up--)
		cteroot = cteroot->parent_root;
	if (cteroot == NULL)
		return NULL;
	foreach_node(CommonTableExpr, x, cteroot->parse->cteList)
	{
		if (strcmp(x->ctename, rte->ctename) == 0)
		{
			cte = x;
			break;
		}
		ndx++;
	}
	if (cte == NULL || cte->cterecursive || cte->search_clause || cte->cycle_clause || ndx >= list_length(cteroot->cte_plan_ids))
		return NULL;
	return list_nth(root->glob->subroots, list_nth_int(cteroot->cte_plan_ids, ndx) - 1);
}

/* the subqueries of a UNION ALL: a member that is itself a UNION ALL (a subquery of arms in an arm's place) gives its own */
static bool
append_arms(PlannerInfo *root, Index parent, List **arms)
{
	foreach_node(AppendRelInfo, ai, root->append_rel_list)
	{
		RelOptInfo *rel;
		int			i = 0;

		if (ai->parent_relid != parent)
			continue;
		rel = root->simple_rel_array[ai->child_relid];
		if (rel == NULL)
			return false;
		foreach_node(RestrictInfo, rinfo, rel->baserestrictinfo)
			if (!rinfo->pseudoconstant)
				return false;
		foreach_ptr(Node, tv, ai->translated_vars)
			if (tv == NULL || !IsA(tv, Var) || ((Var *) tv)->varno != ai->child_relid || ((Var *) tv)->varattno != ++i)
				return false;
		if (rel->subroot != NULL)
			*arms = lappend(*arms, rel->subroot);
		else if (!rt_fetch(ai->child_relid, root->parse->rtable)->inh || !append_arms(root, ai->child_relid, arms))
			return false;
	}
	return true;
}

/*
 * The arms of the outer query's one FROM item: the subqueries of a UNION ALL (appendrel) or the subquery itself. The quals of the
 * outer query that PostgreSQL pushed into the arms (to their WHERE, or HAVING) are theirs now: an arm whose rel still has a qual
 * (one it did not push: volatile, a one-time filter) is not one.
 */
static List *
outer_arms(PlannerInfo *root, Index srti, List **gates)
{
	List	   *arms = NIL;
	RelOptInfo *parent = root->simple_rel_array[srti];

	if (parent == NULL)
		return NIL;
	/* the conditions without a column stay above the arms as PostgreSQL's one-time filter: the scan takes them as its own gate */
	foreach_node(RestrictInfo, rinfo, parent->baserestrictinfo)
		if (rinfo->pseudoconstant)
			*gates = lappend(*gates, rinfo->clause);
		else if (!rt_fetch(srti, root->parse->rtable)->inh)
			return NIL;
	if (!rt_fetch(srti, root->parse->rtable)->inh)
		return parent->subroot ? list_make1(parent->subroot) : NIL;
	return append_arms(root, srti, &arms) ? arms : NIL;
}

/*
 * The table an arm reads: through its CTE scan to the common table expression's root, or the arm's own root (its FROM is the scan itself:
 * a table with its unnests and lookups, which is what an arm over a common table expression that PostgreSQL inlined looks like); NULL if
 * the arm is not a simple aggregate
 */
static PlannerInfo *
arm_base(PlannerInfo *arm, ArmIn *in)
{
	Query	   *q = arm->parse;
	RangeTblEntry *rte;

	if (!q->hasAggs || q->groupClause == NIL || q->groupingSets || q->hasWindowFuncs || q->hasTargetSRFs ||
		q->distinctClause || q->sortClause || q->limitCount || q->limitOffset || q->groupDistinct || q->hasForUpdate ||
		q->setOperations || q->cteList != NIL)
		return NULL;
	in->sub.rti = 0;
	if (list_length(q->jointree->fromlist) != 1 || !IsA(linitial(q->jointree->fromlist), RangeTblRef))
		return arm;
	rte = rt_fetch(((RangeTblRef *) linitial(q->jointree->fromlist))->rtindex, q->rtable);
	if (rte->rtekind != RTE_CTE)
		return arm;
	in->sub.rti = ((RangeTblRef *) linitial(q->jointree->fromlist))->rtindex;
	return cte_root(arm, rte);
}

/*
 * Two arms that read their own copy of one scan (a common table expression that PostgreSQL inlined into each): the same tables and the same
 * WHERE. The entries of pulled up subqueries and of the grouping belong to their arm, and the column names a plan prunes differ: none counts.
 */
static bool
arm_private_rte(RTEKind kind)
{
#if PG_VERSION_NUM >= 180000
	if (kind == RTE_GROUP)
		return true;
#endif
	return kind == RTE_SUBQUERY;
}

static bool
same_scan(PlannerInfo *a, PlannerInfo *b)
{
	ListCell   *la,
			   *lb;

	if (list_length(a->parse->rtable) != list_length(b->parse->rtable) || !equal(a->parse->jointree, b->parse->jointree))
		return false;
	forboth(la, a->parse->rtable, lb, b->parse->rtable)
	{
		RangeTblEntry same = *(RangeTblEntry *) lfirst(la),
				   *rb = lfirst(lb);

		same.eref = rb->eref;
		if (same.rtekind != rb->rtekind || (!arm_private_rte(same.rtekind) && !equal(&same, rb)))
			return false;
	}
	return true;
}

static bool
null_const(Node *e)
{
	return IsA(e, Const) && ((Const *) e)->constisnull;
}

/* `operand op constant` (either side) on an int8 or float8 operand: the operand, the comparison and the constant in *out */
static bool
cond_split(Node *q, Node **operand, SiftAggCond *out)
{
	Oid			lt,
				rt,
				vtype;
	OpExpr	   *op;
	Node	   *l,
			   *r;
	Const	   *k;
	int			cmp;

	if (!IsA(q, OpExpr) || list_length(((OpExpr *) q)->args) != 2)
		return false;
	op = (OpExpr *) q;
	if ((cmp = builtin_cmp(op->opno, &lt, &rt)) < 0)
		return false;
	l = strip_relabel(linitial(op->args));
	r = strip_relabel(lsecond(op->args));
	if (IsA(r, Const))
	{
		*operand = l;
		k = (Const *) r;
	}
	else if (IsA(l, Const))
	{
		*operand = r;
		k = (Const *) l;
		cmp = commute(cmp);
	}
	else
		return false;
	vtype = exprType(*operand);
	out->op = cmp;
	if (k->constisnull)
		return false;
	if (vtype == FLOAT8OID && k->consttype == FLOAT8OID)
	{
		out->isint = false;
		out->cd = DatumGetFloat8(k->constvalue);
	}
	else if (vtype == INT8OID && (k->consttype == INT8OID || k->consttype == INT4OID || k->consttype == INT2OID))
	{
		out->isint = true;
		out->ci = k->consttype == INT8OID ? DatumGetInt64(k->constvalue) : k->consttype == INT4OID ?
			DatumGetInt32(k->constvalue) : DatumGetInt16(k->constvalue);
	}
	else
		return false;
	return true;
}

/* `column op constant` on an output column of the arms: the column in out->col */
static bool
outer_cond(Node *q, Index srti, SiftAggCond *out)
{
	Node	   *x;

	if (!cond_split(q, &x, out) || !IsA(x, Var) || ((Var *) x)->varno != srti || ((Var *) x)->varlevelsup != 0)
		return false;
	out->col = ((Var *) x)->varattno - 1;
	return true;
}

/* the inner aggregate e of the arm, added to its inner aggregates: its index; -1 if the arm cannot give it */
static int
arm_inner_agg(AggCtx *c, ArmIn *in, Node *e, Oid want_type)
{
	SiftAggArm *a = in->out;
	SiftAggAgg *ia = &a->inner[a->ninner];

	if (!IsA(e, Aggref) || exprType(e) != want_type || !compile_agg(c, (Aggref *) e, ia, -1))
		return -1;
	switch (ia->kind)
	{
		case SAA_COUNT_STAR:
		case SAA_COUNT:
		case SAA_SUM_INT:
		case SAA_SUM_F64:
		case SAA_AVG_F64:
			break;
		case SAA_X_SUM:
		case SAA_X_AVG:
			/* the expression is evaluated for every tuple of the scan: an arm's own WHERE or a FILTER would keep it off the tuples they drop */
			if (ia->restype != FLOAT8OID || in->quals != NIL || ia->filter != NULL)
				return -1;
			break;
		default:
			return -1;
	}
	return a->ninner++;
}

/* the inner aggregate that is the arm's column `col`, compiled once: its index in the arm; -1 if the arm cannot give it */
static int
arm_inner(AggCtx *c, ArmIn *in, int col, Oid want_type)
{
	SiftAggArm *a = in->out;

	if (a->colsrc[col] < 0)
		a->colsrc[col] = arm_inner_agg(c, in, list_nth(in->cols, col), want_type);
	return a->colsrc[col];
}

/* a conjunct of the arm's HAVING: `inner aggregate op constant` (in terms of the scanned table), with the aggregate's index as cond.col */
static bool
arm_having(AggCtx *c, ArmIn *in, Node *q, SiftAggCond *out)
{
	Node	   *x;

	return cond_split(q, &x, out) && (out->col = arm_inner_agg(c, in, x, exprType(x))) >= 0;
}

/* the conditions of the outer query without a column, which PostgreSQL left above the arms, as conjuncts of the scan */
static bool
arm_gates(AggCtx *c, SiftAggSpec *s, List *gates)
{
	s->pred = repalloc(s->pred, Max(s->npred + list_length(gates), 1) * sizeof(SiftAggPred *));
	foreach_ptr(Node, q, gates)
	{
		SiftAggPred *p = compile_pred(c, normalize_expr(c, q));

		if (p == NULL || p->kind != SAP_GATE)
			return false;
		s->pred[s->npred++] = p;
		s->desc_quals = psprintf("%s%s%s", s->desc_quals, *s->desc_quals ? " AND " : "", p->desc);
	}
	return true;
}

typedef struct HavingSub
{
	AggCtx	   *c;
	ArmIn	   *in;
	List	   *src;			/* inner aggregate of each Param made */
	bool		bad;
} HavingSub;

/* the inner aggregates of a HAVING conjunct become Params; a column (a group expression) or a volatile function is not allowed */
static Node *
having_mutator(Node *n, HavingSub *h)
{
	if (n == NULL)
		return NULL;
	if (IsA(n, Aggref))
	{
		int			ai = h->bad || list_length(h->src) >= SIFT_AGG_MAX_PARAMS ? -1 : arm_inner_agg(h->c, h->in, n, exprType(n));
		Param	   *p = makeNode(Param);

		if (ai < 0)
			h->bad = true;
		p->paramkind = PARAM_EXEC;
		p->paramid = list_length(h->src);
		p->paramtype = exprType(n);
		p->paramtypmod = -1;
		p->paramcollid = InvalidOid;
		p->location = -1;
		h->src = lappend_int(h->src, ai);
		return (Node *) p;
	}
	if (IsA(n, Var) || (IsA(n, Param) && ((Param *) n)->paramkind == PARAM_EXEC) || IsA(n, SubPlan) || IsA(n, WindowFunc) || IsA(n, GroupingFunc))
		h->bad = true;
	return expression_tree_mutator(n, having_mutator, h);
}

/* the conjuncts of the arm's HAVING that are not `inner aggregate op constant`: PostgreSQL evaluates them on the values of each cell */
static bool
arm_having_expr(AggCtx *c, ArmIn *in, List *quals)
{
	HavingSub	h = {c, in, NIL, false};
	SiftAggArm *a = in->out;

	if (contain_volatile_functions((Node *) quals))
		return false;
	a->hexpr = having_mutator((Node *) make_ands_explicit(quals), &h);
	a->nhp = list_length(h.src);
	a->hsrc = palloc(Max(a->nhp, 1) * sizeof(int));
	foreach_int(ai, h.src)
		a->hsrc[foreach_current_index(ai)] = ai;
	return !h.bad;
}

static bool
count_aggs_walker(Node *n, int *count)
{
	if (n == NULL)
		return false;
	if (IsA(n, Aggref))
		(*count)++;
	return expression_tree_walker(n, count_aggs_walker, count);
}

/* The outer aggregate: avg/sum of a float8 or int8 column of the arms, count(DISTINCT id), count(*), with a FILTER of comparisons of columns */
static bool
outer_agg(AggCtx *c, SiftAggSpec *s, Aggref *oa, Index srti, ArmIn *arms, int narms, SiftAggAgg *out)
{
	Oid			fid = oa->aggfnoid;
	Node	   *arg = NULL;
	Var		   *v = NULL;
	List	   *conds = oa->aggfilter ? make_ands_implicit(oa->aggfilter) : NIL;
	int			nc = 0;

	if (oa->aggkind != AGGKIND_NORMAL || oa->aggorder != NIL || oa->agglevelsup != 0 || oa->aggvariadic || contain_volatile_functions((Node *) oa))
		return false;
	out->leaf = -1;
	out->col = -1;
	out->restype = oa->aggtype;
	out->cond = palloc0(Max(list_length(conds), 1) * sizeof(SiftAggCond));
	foreach_ptr(Node, q, conds)
	{
		if (!outer_cond(q, srti, &out->cond[nc++]))
			return false;
	}
	out->ncond = nc;
	for (int i = 0; i < nc; i++)
		if (out->cond[i].col < 0 || out->cond[i].col >= list_length(arms[0].cols))
			return false;
	if (fid != F_COUNT_)
	{
		if (list_length(oa->args) != 1)
			return false;
		arg = strip_relabel((Node *) linitial_node(TargetEntry, oa->args)->expr);
		if (!IsA(arg, Var) || ((Var *) arg)->varno != srti || ((Var *) arg)->varlevelsup != 0)
			return false;
		v = (Var *) arg;
		out->col = v->varattno - 1;
		if (out->col < 0 || out->col >= list_length(arms[0].cols))
			return false;
	}
	if (fid == F_COUNT_ && oa->aggdistinct == NIL)
		out->kind = SAA_O_COUNT_STAR;
	else if (fid == F_COUNT_ANY && oa->aggdistinct != NIL)
	{
		out->kind = SAA_O_COUNT_ROWS;
		for (int k = 0; k < narms; k++)
			if (!equal(list_nth(arms[k].cols, out->col), arms[k].id))
				return false;
	}
	else if (oa->aggdistinct == NIL && (fid == F_AVG_FLOAT8 || fid == F_SUM_FLOAT8) && v && v->vartype == FLOAT8OID)
		out->kind = fid == F_AVG_FLOAT8 ? SAA_O_AVG_F64 : SAA_O_SUM_F64;
	else if (oa->aggdistinct == NIL && fid == F_SUM_INT8 && v && v->vartype == INT8OID)
		out->kind = SAA_O_SUM_I64;
	else
		return false;
	for (int k = 0; k < narms; k++)
	{
		if (out->kind == SAA_O_AVG_F64 || out->kind == SAA_O_SUM_F64 || out->kind == SAA_O_SUM_I64)
			if (arm_inner(c, &arms[k], out->col, v->vartype) < 0)
				return false;
		for (int i = 0; i < out->ncond; i++)
			if (arm_inner(c, &arms[k], out->cond[i].col, out->cond[i].isint ? INT8OID : FLOAT8OID) < 0)
				return false;
	}
	out->desc = psprintf("%s", out->kind == SAA_O_COUNT_STAR ? "count(*)" : out->kind == SAA_O_COUNT_ROWS ? "count(DISTINCT row)" :
						 out->kind == SAA_O_AVG_F64 ? "avg(inner)" : out->kind == SAA_O_SUM_F64 ? "sum(inner)" : "sum(inner int8)");
	return true;
}

/*
 * A cell is an inner group, the share of one row in one group of an arm. When an arm groups by more than the outer keys and its id, one
 * row has several cells in an outer group, so the group expressions that are no outer key are keys of the cells: one key each, shared by
 * the arms that have an equal expression, after the outer keys and before the number of the set. They name no group (a group has id 0
 * for them, and the scan emits NULL).
 */
static bool
hidden_keys(AggCtx *c, SiftAggSpec *s, ArmIn *arms, int narms, List **scan_tlist, bool sets)
{
	int			nouter = s->nkeys;
	List	   *defs = NIL;

	for (int k = 0; k < narms; k++)
		for (int i = 0; i < list_length(arms[k].group); i++)
		{
			Node	   *g = list_nth(arms[k].group, i);
			SortGroupClause *sgc = list_nth_node(SortGroupClause, arms[k].gsgc, i);
			int			pos;
			SiftAggKey *key;

			if (equal(g, arms[k].id) || list_member(arms[k].used, g))
				continue;
			pos = -1;
			foreach_ptr(Node, d, defs)
				if (equal(d, g))
					pos = foreach_current_index(d);
			if (pos < 0)
			{
				if (s->nkeys + sets >= SIFT_AGG_MAX_KEYS)
					return false;
				key = &s->key[s->nkeys];
				if (!compile_key(c, sgc, g, key))
					return false;
				combo_set_key(c, key, s->nkeys);
				*scan_tlist = lappend(*scan_tlist, makeTargetEntry((Expr *) makeNullConst(key->typid, key->typmod, key->collid),
																   list_length(*scan_tlist) + 1, NULL, false));
				defs = lappend(defs, g);
				pos = s->nkeys++ - nouter;
			}
			arms[k].out->hmask |= (uint32) 1 << (nouter + pos);
		}
	s->nhidden = s->nkeys - nouter;
	return true;
}

/*
 * The outer query aggregates the rows of its FROM item, the arms: a UNION ALL of aggregates (or one) of the same scan, each grouped by
 * a unique row id and by outer keys (a NULL constant where an arm has none of them): see SiftAggArm. The arms are read from their
 * planned roots, so that group expressions and constants are in the planner's final form; an arm that reads a common table
 * expression is looked through to it.
 */
void
sift_agg_two_level(PlannerInfo *root, RelOptInfo *input_rel, RelOptInfo *output_rel, GroupPathExtraData *gextra)
{
	Query	   *parse = root->parse;
	Index		srti = ((RangeTblRef *) linitial(parse->jointree->fromlist))->rtindex;
	List	   *roots;
	ArmIn		arms[AGG_MAX_ARMS] = {0};
	int			narms = 0;
	PlannerInfo *base = NULL;
	AggCtx		ctx = {0},
			   *c = &ctx;
	FromScan	fs = {.ok = true};
	SiftAggSpec *s;
	OutWalk		ow = {NIL, NIL, false};
	List	   *scan_tlist = NIL;
	int			okey[SIFT_AGG_MAX_KEYS];
	int			ncols = 0;
	List	   *gates = NIL;
	int			nkeyslots = list_length(parse->groupClause) + 1;

	if (list_length(parse->groupClause) > SIFT_AGG_MAX_KEYS - 1)
		return;
	roots = outer_arms(root, srti, &gates);
	if (roots == NIL || list_length(roots) > AGG_MAX_ARMS)
		return;
	foreach_ptr(PlannerInfo, ar, roots)
	{
		PlannerInfo *b = arm_base(ar, &arms[narms]);

		if (b == NULL || (base != NULL && b != base && !(b == ar && same_scan(base, b))))
			return;
		if (base == NULL)
			base = b;
		arms[narms].sub.tlist = b->parse->targetList;
		narms++;
	}
	if (!agg_base(c, base, NULL, &fs) || c->nparts > 0)
		return;
	s = agg_spec_new(c);
	if (!agg_quals(c, s, &fs) || !arm_gates(c, s, gates))
		return;

	/* the arms in the terms of the scanned table */
	for (int k = 0; k < narms; k++)
	{
		Query	   *aq = ((PlannerInfo *) list_nth(roots, k))->parse;
		ArmIn	   *in = &arms[k];
		int			nid = 0,
					nagg;
		List	   *rest = NIL;

		foreach_node(TargetEntry, te, aq->targetList)
			if (!te->resjunk)
				in->cols = lappend(in->cols, normalize_expr(c, cte_sub_mutator((Node *) te->expr, &in->sub)));
		if (k == 0)
			ncols = list_length(in->cols);
		else if (ncols != list_length(in->cols))
			return;
		foreach_node(SortGroupClause, sgc, aq->groupClause)
		{
			Node	   *g = normalize_expr(c, cte_sub_mutator((Node *) get_sortgroupclause_expr(sgc, aq->targetList), &in->sub));

			in->group = lappend(in->group, g);
			in->gsgc = lappend(in->gsgc, sgc);
			if (unique_row_column(c, g))
			{
				in->id = g;
				nid++;
			}
		}
		if (nid != 1)
			return;
		if (in->sub.rti != 0 && aq->jointree->quals != NULL)
			foreach_ptr(Node, q, (List *) aq->jointree->quals)
				in->quals = lappend(in->quals, normalize_expr(c, cte_sub_mutator(q, &in->sub)));
		in->out = palloc0(sizeof(SiftAggArm));
		in->out->colsrc = palloc(Max(ncols, 1) * sizeof(int));
		for (int i = 0; i < ncols; i++)
			in->out->colsrc[i] = -1;
		nagg = ncols;
		count_aggs_walker(aq->havingQual, &nagg);
		in->out->inner = palloc0((nagg + 1) * sizeof(SiftAggAgg));
		in->out->having = palloc0(Max(list_length((List *) aq->havingQual), 1) * sizeof(SiftAggCond));
		foreach_ptr(Node, q, (List *) aq->havingQual)
		{
			Node	   *hq = normalize_expr(c, cte_sub_mutator(q, &in->sub));

			if (arm_having(c, in, hq, &in->out->having[in->out->nhaving]))
				in->out->nhaving++;
			else
				rest = lappend(rest, hq);
		}
		if (rest != NIL && !arm_having_expr(c, in, rest))
			return;
		in->out->filter = palloc(Max(list_length(in->quals), 1) * sizeof(SiftAggPred *));
		foreach_ptr(Node, q, in->quals)
		{
			SiftAggPred *p = compile_combo_pred(c, q);

			if (p == NULL || unsupported_clause(c, q))
				return;
			in->out->filter[in->out->nfilter++] = p;
		}
	}

	/* the outer keys: columns that every arm groups by or has as NULL */
	for (int k = 0; k < narms; k++)
		nkeyslots += list_length(arms[k].group);
	s->key = palloc0(nkeyslots * sizeof(SiftAggKey));
	foreach_node(SortGroupClause, sgc, parse->groupClause)
	{
		Node	   *e = (Node *) get_sortgroupclause_expr(sgc, root->processed_tlist);
		Node	   *def = NULL;

		if (!IsA(e, Var) || ((Var *) e)->varno != srti || ((Var *) e)->varlevelsup != 0 || ((Var *) e)->varattno > ncols)
			return;
		okey[s->nkeys] = ((Var *) e)->varattno - 1;
		for (int k = 0; k < narms; k++)
		{
			Node	   *x = list_nth(arms[k].cols, okey[s->nkeys]);

			if (null_const(x))
				continue;
			if (!list_member(arms[k].group, x) || equal(x, arms[k].id) || (def != NULL && !equal(x, def)))
				return;
			def = x;
			arms[k].used = lappend(arms[k].used, x);
			arms[k].out->keymask |= (uint32) 1 << s->nkeys;
		}
		if (def == NULL || !compile_key(c, sgc, def, &s->key[s->nkeys]))
			return;
		combo_set_key(c, &s->key[s->nkeys], s->nkeys);
		ow.keys = lappend(ow.keys, e);
		scan_tlist = lappend(scan_tlist, makeTargetEntry((Expr *) e, list_length(scan_tlist) + 1, NULL, false));
		s->nkeys++;
	}
	if (!hidden_keys(c, s, arms, narms, &scan_tlist, parse->groupingSets != NIL))
		return;
	if (parse->groupingSets != NIL)
		add_sets(parse, s, &scan_tlist);

	/* the outer aggregates */
	foreach_ptr(Node, e, output_rel->reltarget->exprs)
		out_walker(e, &ow);
	if (gextra && gextra->havingQual)
		out_walker((Node *) gextra->havingQual, &ow);
	if (ow.bad)
		return;
	s->agg = palloc0(Max(list_length(ow.aggs), 1) * sizeof(SiftAggAgg));
	foreach_ptr(Node, a, ow.aggs)
	{
		if (!(IsA(a, GroupingFunc) ? compile_grouping(parse, (GroupingFunc *) a, &s->agg[s->naggs]) :
			  IsA(a, Aggref) && outer_agg(c, s, (Aggref *) a, srti, arms, narms, &s->agg[s->naggs])))
			return;
		scan_tlist = lappend(scan_tlist, makeTargetEntry((Expr *) a, list_length(scan_tlist) + 1, NULL, false));
		s->naggs++;
	}
	s->narms = narms;
	s->arm = palloc0(narms * sizeof(SiftAggArm));
	for (int k = 0; k < narms; k++)
	{
		s->arm[k] = *arms[k].out;
		s->arm[k].ncol = ncols;
	}
	agg_finish(c, s, root, input_rel, output_rel, output_rel->reltarget, ow.keys, scan_tlist);
}
