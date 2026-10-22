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
#include "optimizer/prep.h"
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
	List	   *kexpr;			/* the group expressions that are a key of the scan (outer or hidden) ... */
	List	   *kidx;			/* ... and their keys */
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
	Relids		live;
	Index		rti = 0;

	if (list_length(a->parse->rtable) != list_length(b->parse->rtable) || !equal(a->parse->jointree, b->parse->jointree))
		return false;
	live = get_relids_in_jointree((Node *) a->parse->jointree, false, false);
	forboth(la, a->parse->rtable, lb, b->parse->rtable)
	{
		RangeTblEntry same = *(RangeTblEntry *) lfirst(la),
				   *rb = lfirst(lb);

		same.eref = rb->eref;
		rti++;
		/* a sub-select that was not pulled up is joined: its rows are the arm's lookup, so its query must be the same */
		if (same.rtekind != rb->rtekind || (!arm_private_rte(same.rtekind) && !equal(&same, rb)) ||
			(same.rtekind == RTE_SUBQUERY && bms_is_member(rti, live) && !equal(same.subquery, rb->subquery)))
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

/* the key of the scan that group expression x of the arm is; -1 if it is none */
static int
arm_key_of(const ArmIn *arm, Node *x)
{
	ListCell   *le,
			   *li;

	forboth(le, arm->kexpr, li, arm->kidx)
		if (equal(lfirst(le), x))
			return lfirst_int(li);
	return -1;
}

/*
 * count(DISTINCT x) of a column that every arm groups by (or has as NULL): the distinct ids of that key among the cells of a group. The
 * key is one of the scan, with the ids of the grouping, so the distinct operator must be the key's.
 */
static int
distinct_key(SiftAggSpec *s, Aggref *oa, ArmIn *arms, int narms, int col)
{
	int			key = -1;
	SortGroupClause *sgc;

	for (int k = 0; k < narms; k++)
	{
		Node	   *x = list_nth(arms[k].cols, col);
		int			j;

		if (null_const(x))
			continue;
		j = arm_key_of(&arms[k], x);
		if (j < 0 || (key >= 0 && j != key))
			return -1;
		key = j;
	}
	if (key < 0 || list_length(oa->aggdistinct) != 1)
		return -1;
	sgc = linitial_node(SortGroupClause, oa->aggdistinct);
	return sgc->eqop == s->key[key].eqop && oa->inputcollid == s->key[key].collid ? key : -1;
}

/* The outer aggregate: avg/sum of a float8 or int8 column of the arms, count(DISTINCT id or a key), count(*), with a FILTER of comparisons of columns */
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
		bool		ids = true;

		for (int k = 0; k < narms; k++)
			ids &= equal(list_nth(arms[k].cols, out->col), arms[k].id);
		if (ids)
			out->kind = SAA_O_COUNT_ROWS;
		else if ((out->slot = distinct_key(s, oa, arms, narms, out->col)) >= 0)
			out->kind = SAA_O_COUNT_KEY;
		else
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
						 out->kind == SAA_O_COUNT_KEY ? "count(DISTINCT key)" :
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
				if (s->nkeys + sets >= sift_limit(group_keys, SIFT_AGG_MAX_KEYS))
					return agg_refuse_limit(TOO_MANY_KEYS, group_keys, sift_limit(group_keys, SIFT_AGG_MAX_KEYS), "%d keys with the hidden ones of the arms", s->nkeys + sets + 1);
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
			arms[k].kexpr = lappend(arms[k].kexpr, g);
			arms[k].kidx = lappend_int(arms[k].kidx, nouter + pos);
		}
	s->nhidden = s->nkeys - nouter;
	return true;
}

/* What planning one two level aggregate builds up: the arms, the spec and the scan's target list */
typedef struct TwoLevel
{
	PlannerInfo *root;
	Index		srti;			/* the outer query's FROM item: the arms */
	JoinExpr   *join;			/* the arms are joined to a spine; NULL: they are the FROM item */
	Index		spine_rti;
	List	   *spine_cols;		/* the Vars of the spine that the join and the keys read: the columns of its plan */
	SiftAggSpine *spine;
	int			pair_att[SIFT_AGG_MAX_JKEYS];	/* the column of the arms each pair of the join reads */
	List	   *roots;			/* the planned roots of the arms */
	List	   *gates;
	ArmIn		arms[SIFT_AGG_MAX_ARMS];
	int			narms;
	int			ncols;			/* the columns every arm returns */
	AggCtx		ctx;
	SiftAggSpec *s;
	OutWalk		ow;
	List	   *scan_tlist;
} TwoLevel;

/* The arms of the FROM item and the scan they share; NULL when they are not arms of one scan */
static PlannerInfo *
two_arms(TwoLevel *t)
{
	PlannerInfo *base = NULL;

	t->roots = outer_arms(t->root, t->srti, &t->gates);
	if (t->roots == NIL || agg_width_hit(list_length(t->roots) - 1, arms, sift_limit(arms, SIFT_AGG_MAX_ARMS), "arms"))
		return NULL;
	foreach_ptr(PlannerInfo, ar, t->roots)
	{
		PlannerInfo *b = arm_base(ar, &t->arms[t->narms]);

		if (b == NULL || (base != NULL && b != base && !(b == ar && same_scan(base, b))))
			return NULL;
		if (base == NULL)
			base = b;
		t->arms[t->narms].sub.tlist = b->parse->targetList;
		t->narms++;
	}
	return base;
}

/* arm k in the terms of the scanned table: its columns, group expressions, filter and HAVING */
static bool
two_arm_terms(TwoLevel *t, int k)
{
	AggCtx	   *c = &t->ctx;
	Query	   *aq = ((PlannerInfo *) list_nth(t->roots, k))->parse;
	ArmIn	   *in = &t->arms[k];
	int			nid = 0,
				nagg;
	List	   *rest = NIL;

	foreach_node(TargetEntry, te, aq->targetList)
		if (!te->resjunk)
			in->cols = lappend(in->cols, normalize_expr(c, cte_sub_mutator((Node *) te->expr, &in->sub)));
	if (k == 0)
		t->ncols = list_length(in->cols);
	else if (t->ncols != list_length(in->cols))
		return false;
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
		return false;
	if (in->sub.rti != 0 && aq->jointree->quals != NULL)
		foreach_ptr(Node, q, (List *) aq->jointree->quals)
			in->quals = lappend(in->quals, normalize_expr(c, cte_sub_mutator(q, &in->sub)));
	in->out = palloc0(sizeof(SiftAggArm));
	in->out->colsrc = palloc(Max(t->ncols, 1) * sizeof(int));
	for (int i = 0; i < t->ncols; i++)
		in->out->colsrc[i] = -1;
	nagg = t->ncols;
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
		return false;
	in->out->filter = palloc(Max(list_length(in->quals), 1) * sizeof(SiftAggPred *));
	foreach_ptr(Node, q, in->quals)
	{
		SiftAggPred *p = compile_combo_pred(c, q);

		if (p == NULL || unsupported_clause(c, q))
			return false;
		in->out->filter[in->out->nfilter++] = p;
	}
	return true;
}

/* a column every arm has as NULL is a key no arm groups by: no group has an id for it, and the scan emits NULL */
static void
null_key(SortGroupClause *sgc, Node *e, SiftAggKey *k)
{
	/* the key has no program: no arm selects it, so it is never evaluated */
	k->kind = SAK_NUM;
	k->leaf = -1;
	k->typid = exprType(e);
	k->typmod = exprTypmod(e);
	k->collid = exprCollation(e);
	k->eqop = sgc->eqop;
	k->desc = "NULL";
}

/* the spine's Var: a column of the spine at the query's own level */
static bool
spine_var(const TwoLevel *t, Node *n)
{
	return t->join != NULL && IsA(n, Var) && ((Var *) n)->varno == t->spine_rti && ((Var *) n)->varlevelsup == 0 && ((Var *) n)->varattno > 0;
}

/* the column of the spine's plan that spine Var v is */
static int
spine_col(TwoLevel *t, Var *v)
{
	foreach_ptr(Var, x, t->spine_cols)
		if (x->varattno == v->varattno)
			return foreach_current_index(x);
	t->spine_cols = lappend(t->spine_cols, makeVar(t->spine_rti, v->varattno, v->vartype, v->vartypmod, v->varcollid, 0));
	return list_length(t->spine_cols) - 1;
}

/* a group column of the spine is a key no arm computes: the ids are those of the spine's values */
static bool
spine_key(TwoLevel *t, SortGroupClause *sgc, Var *v, SiftAggKey *k)
{
	SiftAggSpine *sp = t->spine;
	RegProcedure lhs,
				rhs;

	if (sp->nout >= SIFT_AGG_MAX_KEYS || !get_op_hash_functions(sgc->eqop, &lhs, &rhs))
		return false;
	sp->outcol[sp->nout] = spine_col(t, v);
	sp->outkey[sp->nout++] = t->s->nkeys;
	k->kind = SAK_SPINE;
	k->leaf = -1;
	k->typid = v->vartype;
	k->typmod = v->vartypmod;
	k->collid = v->varcollid;
	k->eqop = sgc->eqop;
	k->desc = psprintf("spine.%s", get_rte_attribute_name(rt_fetch(t->spine_rti, t->root->parse->rtable), v->varattno));
	return true;
}

/* a group column of the arms: every arm groups by the same expression or has NULL */
static bool
arm_key(TwoLevel *t, SortGroupClause *sgc, Node *e, SiftAggKey *key)
{
	SiftAggSpec *s = t->s;
	Node	   *def = NULL;
	int			col;

	/* a bucket column of the spine that is NULL, which the planner folded into the query */
	if (t->join != NULL && null_const(e))
	{
		null_key(sgc, e, key);
		return true;
	}
	if (!IsA(e, Var) || ((Var *) e)->varno != t->srti || ((Var *) e)->varlevelsup != 0 || ((Var *) e)->varattno > t->ncols)
		return false;
	col = ((Var *) e)->varattno - 1;
	for (int k = 0; k < t->narms; k++)
	{
		Node	   *x = list_nth(t->arms[k].cols, col);

		if (null_const(x))
			continue;
		if (!list_member(t->arms[k].group, x) || equal(x, t->arms[k].id) || (def != NULL && !equal(x, def)))
			return false;
		def = x;
		t->arms[k].used = lappend(t->arms[k].used, x);
		t->arms[k].kexpr = lappend(t->arms[k].kexpr, x);
		t->arms[k].kidx = lappend_int(t->arms[k].kidx, s->nkeys);
		t->arms[k].out->keymask |= (uint32) 1 << s->nkeys;
	}
	if (def == NULL)
	{
		null_key(sgc, e, key);
		return true;
	}
	return compile_key(&t->ctx, sgc, def, key);
}

/* the outer keys: columns that every arm groups by or has as NULL, and the spine's */
static bool
two_keys(TwoLevel *t)
{
	AggCtx	   *c = &t->ctx;
	SiftAggSpec *s = t->s;
	Query	   *parse = t->root->parse;
	int			nkeyslots = list_length(parse->groupClause) + 1;

	for (int k = 0; k < t->narms; k++)
		nkeyslots += list_length(t->arms[k].group);
	s->key = palloc0(nkeyslots * sizeof(SiftAggKey));
	foreach_node(SortGroupClause, sgc, parse->groupClause)
	{
		Node	   *e = (Node *) get_sortgroupclause_expr(sgc, t->root->processed_tlist);

		if (!(spine_var(t, e) ? spine_key(t, sgc, (Var *) e, &s->key[s->nkeys]) : arm_key(t, sgc, e, &s->key[s->nkeys])))
			return false;
		combo_set_key(c, &s->key[s->nkeys], s->nkeys);
		t->ow.keys = lappend(t->ow.keys, e);
		t->scan_tlist = lappend(t->scan_tlist, makeTargetEntry((Expr *) e, list_length(t->scan_tlist) + 1, NULL, false));
		s->nkeys++;
	}
	if (!hidden_keys(c, s, t->arms, t->narms, &t->scan_tlist, parse->groupingSets != NIL))
		return false;
	if (parse->groupingSets != NIL)
		add_sets(parse, s, &t->scan_tlist);
	return true;
}

/* the outer aggregates: those of the target list and of HAVING */
static bool
two_aggs(TwoLevel *t, RelOptInfo *output_rel, GroupPathExtraData *gextra)
{
	SiftAggSpec *s = t->s;

	foreach_ptr(Node, e, output_rel->reltarget->exprs)
		out_walker(e, &t->ow);
	if (gextra && gextra->havingQual)
		out_walker((Node *) gextra->havingQual, &t->ow);
	if (t->ow.bad)
		return false;
	s->agg = palloc0(Max(list_length(t->ow.aggs), 1) * sizeof(SiftAggAgg));
	foreach_ptr(Node, a, t->ow.aggs)
	{
		if (!(IsA(a, GroupingFunc) ? compile_grouping(t->root->parse, (GroupingFunc *) a, &s->agg[s->naggs]) :
			  IsA(a, Aggref) && outer_agg(&t->ctx, s, (Aggref *) a, t->srti, t->arms, t->narms, &s->agg[s->naggs])))
			return false;
		t->scan_tlist = lappend(t->scan_tlist, makeTargetEntry((Expr *) a, list_length(t->scan_tlist) + 1, NULL, false));
		s->naggs++;
	}
	return true;
}

/* the spine's range table entry: a table, or a sub-select over one with a WHERE that PostgreSQL pulled up (its rows are the rel's filtered ones); 0 if it is not */
static Index
spine_item(Node *n)
{
	if (IsA(n, FromExpr) && list_length(((FromExpr *) n)->fromlist) == 1)
		n = linitial(((FromExpr *) n)->fromlist);
	return IsA(n, RangeTblRef) ? ((RangeTblRef *) n)->rtindex : 0;
}

/* the WHERE of the query reads the spine only (PostgreSQL moved the WHERE of a spine sub-select up): the rows of the spine are the filtered ones */
static bool
spine_where(PlannerInfo *root, Index spine)
{
	foreach_ptr(Node, q, (List *) root->parse->jointree->quals)
		if (!bms_equal(pull_varnos(root, q), bms_make_singleton(spine)))
			return false;
	return true;
}

/*
 * The FROM item is the arms, or `spine JOIN arms ON equalities` (either order) or `spine LEFT JOIN arms ON equalities`; the WHERE would
 * filter the joined rows, so it reads the spine only. A subquery PostgreSQL did not pull up and a plan of a few rows are what the spine can be.
 */
static bool
two_from(TwoLevel *t)
{
	Query	   *parse = t->root->parse;
	Node	   *from = linitial(parse->jointree->fromlist);
	JoinExpr   *j;
	List	   *gates = NIL;

	if (list_length(parse->jointree->fromlist) != 1)
		return false;
	if (IsA(from, RangeTblRef))
	{
		t->srti = ((RangeTblRef *) from)->rtindex;
		return true;
	}
	while (IsA(from, FromExpr) && list_length(((FromExpr *) from)->fromlist) == 1 && ((FromExpr *) from)->quals == NULL)
		from = linitial(((FromExpr *) from)->fromlist);
	if (!IsA(from, JoinExpr))
		return false;
	j = (JoinExpr *) from;
	if (j->jointype != JOIN_INNER && j->jointype != JOIN_LEFT)
		return false;
	/* the preserved side of a LEFT JOIN is the left one: the spine */
	for (int side = 0; side < (j->jointype == JOIN_LEFT ? 1 : 2); side++)
	{
		Node	   *a = side == 0 ? j->rarg : j->larg,
				   *b = side == 0 ? j->larg : j->rarg;
		Index		spine = spine_item(b);

		if (IsA(a, RangeTblRef) && spine != 0 && rt_fetch(((RangeTblRef *) a)->rtindex, parse->rtable)->rtekind == RTE_SUBQUERY &&
			spine_where(t->root, spine) && outer_arms(t->root, ((RangeTblRef *) a)->rtindex, &gates) != NIL)
		{
			if (parse->groupingSets != NIL)
				return agg_refuse(GROUPING_SETS);
			t->join = j;
			t->srti = ((RangeTblRef *) a)->rtindex;
			t->spine_rti = spine;
			t->spine = palloc0(sizeof(SiftAggSpine));
			t->spine->left = j->jointype == JOIN_LEFT;
			return true;
		}
	}
	return false;
}

static bool
arm_var(const TwoLevel *t, Node *n)
{
	return IsA(n, Var) && ((Var *) n)->varno == t->srti && ((Var *) n)->varlevelsup == 0 && ((Var *) n)->varattno > 0 &&
		((Var *) n)->varattno <= t->ncols;
}

/* the types whose equality is that of their JSON images, which the jsonb arrays of a join compare */
static bool
spine_type(Oid typid, Oid collid)
{
	switch (typid)
	{
		case BOOLOID:
		case INT2OID:
		case INT4OID:
		case INT8OID:
		case NUMERICOID:
		case DATEOID:
		case TIMESTAMPOID:
		case TIMESTAMPTZOID:
			return true;
		case TEXTOID:
		case VARCHAROID:
			return !OidIsValid(collid) || get_collation_isdeterministic(collid);
		default:
			return false;
	}
}

/* `arm column = spine column or constant` (either order; opno: the equality operator, InvalidOid for the elements of jsonb arrays): a pair of the join */
static bool
spine_eq(TwoLevel *t, Oid opno, Node *a, Node *b, bool nullsafe)
{
	struct SiftAggSpinePair *q;
	Node	   *x = strip_relabel(a),
			   *y = strip_relabel(b);
	TypeCacheEntry *tc;

	if (!arm_var(t, x))
	{
		Node	   *z = x;

		x = y;
		y = z;
	}
	if (!arm_var(t, x) || !(spine_var(t, y) || IsA(y, Const)) || t->spine->npair >= SIFT_AGG_MAX_JKEYS ||
		exprType(x) != exprType(y) || exprCollation(x) != exprCollation(y) || !spine_type(exprType(x), exprCollation(x)))
		return false;
	tc = lookup_type_cache(exprType(x), TYPECACHE_EQ_OPR);
	if (!OidIsValid(tc->eq_opr) || (OidIsValid(opno) && opno != tc->eq_opr))
		return false;
	q = &t->spine->pair[t->spine->npair];
	q->col = IsA(y, Var) ? spine_col(t, (Var *) y) : -1;
	q->konst = IsA(y, Const) ? y : NULL;
	q->nullsafe = nullsafe;
	q->typid = exprType(x);
	q->collid = exprCollation(x);
	q->eqop = tc->eq_opr;
	t->pair_att[t->spine->npair++] = ((Var *) x)->varattno;
	return true;
}

/* a conjunct of the join: an equality, `IS NOT DISTINCT FROM`, or the equality of two jsonb arrays (each element null-safe) */
static bool
spine_conjunct(TwoLevel *t, Node *q)
{
	if (IsA(q, OpExpr) && list_length(((OpExpr *) q)->args) == 2)
	{
		OpExpr	   *op = (OpExpr *) q;
		Node	   *l = linitial(op->args),
				   *r = lsecond(op->args);
		ListCell   *la,
				   *lb;

		if (get_opcode(op->opno) != F_JSONB_EQ)
			return spine_eq(t, op->opno, l, r, false);
		if (!IsA(l, FuncExpr) || !IsA(r, FuncExpr) || ((FuncExpr *) l)->funcid != F_JSONB_BUILD_ARRAY_ANY ||
			((FuncExpr *) r)->funcid != F_JSONB_BUILD_ARRAY_ANY || list_length(((FuncExpr *) l)->args) != list_length(((FuncExpr *) r)->args))
			return false;
		forboth(la, ((FuncExpr *) l)->args, lb, ((FuncExpr *) r)->args)
			if (!spine_eq(t, InvalidOid, lfirst(la), lfirst(lb), true))
				return false;
		return true;
	}
	if (IsA(q, BoolExpr) && ((BoolExpr *) q)->boolop == NOT_EXPR && IsA(linitial(((BoolExpr *) q)->args), DistinctExpr))
	{
		DistinctExpr *d = linitial_node(DistinctExpr, ((BoolExpr *) q)->args);

		return list_length(d->args) == 2 && spine_eq(t, d->opno, linitial(d->args), lsecond(d->args), true);
	}
	return false;
}

/* the spine: the pairs of the join resolved to the keys of each arm, and the plan of its rows (the one child of the path) */
static bool
two_spine(TwoLevel *t)
{
	SiftAggSpine *sp = t->spine;
	PlannerInfo *root = t->root;
	RelOptInfo *rel = root->simple_rel_array[t->spine_rti];
	RangeTblEntry *e = rt_fetch(t->spine_rti, root->parse->rtable);
	AggCtx		outer = {.root = root};
	PathTarget *target = create_empty_pathtarget();
	Path	   *p;

	foreach_ptr(Node, q, t->join->quals == NULL ? NIL : IsA(t->join->quals, List) ? (List *) t->join->quals : list_make1(t->join->quals))
		if (!spine_conjunct(t, q))
			return false;
	if (sp->npair == 0)
		return false;
	for (int i = 0; i < sp->npair; i++)
		for (int k = 0; k < t->narms; k++)
		{
			Node	   *x = list_nth(t->arms[k].cols, t->pair_att[i] - 1);

			sp->pair[i].key[k] = null_const(x) ? -1 : arm_key_of(&t->arms[k], x);
			if (sp->pair[i].key[k] < 0 && !null_const(x))
				return false;
		}
	if (rel == NULL || rel->reloptkind != RELOPT_BASEREL || !bms_is_empty(rel->lateral_relids) || rel->cheapest_total_path == NULL ||
		t->spine_cols == NIL)
		return false;
	/* the rows are read once, when the scan starts: a condition of the spine that changes between rescans cannot be */
	foreach_node(RestrictInfo, rinfo, rel->baserestrictinfo)
		if (contain_volatile_functions((Node *) rinfo->clause) || has_exec_param_walker((Node *) rinfo->clause, NULL))
			return false;
	switch (e->rtekind)
	{
		case RTE_RELATION:
			if (e->relkind != RELKIND_RELATION)
				return false;
			break;
		case RTE_CTE:
			{
				PlannerInfo *owner = root;

				for (Index up = e->ctelevelsup; up > 0; up--)
					owner = owner->parent_root;
				if (e->self_reference || cte_owner_correlated(owner))
					return false;
			}
			break;
		case RTE_VALUES:
		case RTE_FUNCTION:
		case RTE_SUBQUERY:
			if (reads_outer(&outer, e, 1) || contain_volatile_functions(e->rtekind == RTE_VALUES ? (Node *) e->values_lists :
																		 e->rtekind == RTE_FUNCTION ? (Node *) e->functions : (Node *) e->subquery))
				return false;
			break;
		default:
			return false;
	}
	foreach_ptr(Var, v, t->spine_cols)
		add_column_to_pathtarget(target, (Expr *) v, 0);
	set_pathtarget_cost_width(root, target);
	p = (Path *) create_projection_path(root, rel, rel->cheapest_total_path, target);
	if (!agg_lookup_fits(p->rows, p->pathtarget->width))
		return agg_refuse_limit(SPINE_ROWS, lookup_kb, sift_agg_lookup_max_kb, "the spine is estimated at %.0f rows of width %d", p->rows, p->pathtarget->width);
	t->ctx.spine = p;
	sp->child = 0;
	sp->ncol = list_length(t->spine_cols);
	return true;
}

/* the join PostgreSQL made of the tables of the arms' FROM, the input of the arms' own aggregates */
static RelOptInfo *
arm_input(PlannerInfo *base)
{
	Relids		all = base->all_query_rels;

	return bms_membership(all) == BMS_SINGLETON ? base->simple_rel_array[bms_singleton_member(all)] : find_join_rel(base, all);
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
	TwoLevel	t = {.root = root};
	AggCtx	   *c = &t.ctx;
	FromScan	fs = {.ok = true};
	PlannerInfo *base;

	if (!two_from(&t))
		return;
	agg_arm_candidate();
	if (list_length(parse->groupClause) > sift_limit(group_keys, SIFT_AGG_MAX_KEYS) - 1)
	{
		agg_refused_limit(TOO_MANY_KEYS, group_keys, sift_limit(group_keys, SIFT_AGG_MAX_KEYS) - 1, "%d group keys, and the arms' row id takes one", list_length(parse->groupClause));
		return;
	}
	if ((base = two_arms(&t)) == NULL)
	{
		agg_refused(TWO_ARMS);
		return;
	}
	c->detached = true;
	if (!agg_base(c, base, arm_input(base), &fs) || c->nparts > 0)
		return;
	t.s = agg_spec_new(c);
	if (!agg_quals(c, t.s, &fs))
		return;
	if (!arm_gates(c, t.s, t.gates))
	{
		agg_refused(TWO_ARMS);
		return;
	}
	for (int k = 0; k < t.narms; k++)
		if (!two_arm_terms(&t, k))
		{
			agg_refused(TWO_ARMS);
			return;
		}
	if (!two_keys(&t))
	{
		agg_refused(TWO_KEYS);
		return;
	}
	if (!two_aggs(&t, output_rel, gextra))
	{
		agg_refused(TWO_AGGS);
		return;
	}
	if (t.join != NULL && !two_spine(&t))
	{
		agg_refused(SPINE_SHAPE);
		return;
	}
	t.s->spine = t.spine;
	t.s->narms = t.narms;
	t.s->arm = palloc0(t.narms * sizeof(SiftAggArm));
	for (int k = 0; k < t.narms; k++)
	{
		t.s->arm[k] = *t.arms[k].out;
		t.s->arm[k].ncol = t.ncols;
	}
	agg_finish(c, t.s, root, input_rel, output_rel, output_rel->reltarget, t.ow.keys, t.scan_tlist);
}
