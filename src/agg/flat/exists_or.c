/*
 * agg/flat/exists_or.c - EXISTS over an OR of key tests is a key in the keys the arms give
 * `EXISTS (SELECT 1 FROM links l WHERE c AND (l.k = d.id OR l.g = ANY(ARRAY(SELECT g.g FROM group_links g WHERE g.k = d.id))))` cannot be pulled up by PostgreSQL: the OR
 * reads the outer row in both arms, so the plan is a semi join with a filter that runs the array's sub-select for every pair of rows (a cliff on small link tables, where the
 * planner costs the pairs cheap). Each arm tests one key of the outer row, so the EXISTS is `d.id IN (keys of the arm 1) OR d.id IN (keys of the arm 2)`: the keys of an arm
 * are `SELECT l.k FROM links l WHERE c`, and for the array of a group `SELECT g.k FROM group_links g WHERE g.g IN (SELECT l.g FROM links l WHERE c)`, sub-selects of no outer
 * row that PostgreSQL hashes once. A NULL key matches nothing in the EXISTS and is left out, and the outer value is tested for NULL, so the answer is true or false as it was.
 */
#include "postgres.h"

#include "catalog/pg_class_d.h"
#include "catalog/pg_type_d.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "optimizer/optimizer.h"
#include "parser/parsetree.h"
#include "rewrite/rewriteManip.h"
#include "utils/lsyscache.h"
#include "utils/typcache.h"

#include "agg/agg.h"

/* one arm of the OR: the outer expression its key is tested against, the key column of the table, and for an array of a group the sub-select that gives the group */
typedef struct Arm
{
	Node	   *outer;			/* over the query that holds the EXISTS (levels already lowered) */
	Var		   *col;			/* the key column of the table of the EXISTS */
	OpExpr	   *eq;				/* its test */
	Query	   *group;			/* the ARRAY sub-select (NULL: the arm is the key itself) */
	Var		   *group_key;		/* its column compared with the outer expression */
	Var		   *group_out;		/* the column it returns */
	List	   *group_rest;		/* its other conditions */
	ScalarArrayOpExpr *any;
} Arm;

typedef struct Outer
{
	int			level;
	bool		found;
} Outer;

/* a Var, a sublink or a volatile call of a higher level: found when it reads a level up from the level of the walk start, `level` counting the nested queries */
static bool
levels_walker(Node *n, Outer *c)
{
	if (n == NULL)
		return false;
	SIFT_SKIP_CONST_LIST(n, false);
	if (IsA(n, Var))
	{
		Var		   *v = (Var *) n;

		if ((int) v->varlevelsup > c->level)
			c->found = true;
		return false;
	}
	if (IsA(n, Query))
	{
		bool		r;

		c->level++;
		r = query_tree_walker((Query *) n, levels_walker, c, 0);
		c->level--;
		return r;
	}
	return expression_tree_walker(n, levels_walker, c);
}

/* does the expression read a variable of the query around it (any level up)? */
static bool
reads_outside(Node *n)
{
	Outer		c = {0, false};

	levels_walker(n, &c);
	return c.found;
}

/* the variables of the expression that reach exactly `up` levels above it (and none reaching further or none of the level itself when only_up) */
typedef struct Reach
{
	int			up;
	bool		same;			/* a variable of the expression's own level */
	bool		other;			/* a variable of another level above (not `up`) */
	bool		hit;			/* a variable at `up` */
	int			level;
} Reach;

static bool
reach_walker(Node *n, Reach *c)
{
	if (n == NULL)
		return false;
	SIFT_SKIP_CONST_LIST(n, false);
	if (IsA(n, Var))
	{
		Var		   *v = (Var *) n;
		int			up = (int) v->varlevelsup - c->level;

		if (up == 0)
			c->same = true;
		else if (up == c->up)
			c->hit = true;
		else if (up > 0)
			c->other = true;
		return false;
	}
	if (IsA(n, Query))
	{
		bool		r;

		c->level++;
		r = query_tree_walker((Query *) n, reach_walker, c, 0);
		c->level--;
		return r;
	}
	return expression_tree_walker(n, reach_walker, c);
}

/* the expression reads variables of exactly the level `up` above it and nothing of its own or another level */
static bool
only_reaches(Node *n, int up)
{
	Reach		c = {up, false, false, false, 0};

	reach_walker(n, &c);
	return c.hit && !c.same && !c.other;
}

static List *
and_list(Node *quals)
{
	if (quals == NULL)
		return NIL;
	if (IsA(quals, BoolExpr) && ((BoolExpr *) quals)->boolop == AND_EXPR)
		return ((BoolExpr *) quals)->args;
	return list_make1(quals);
}

static Node *
strip(Node *n)
{
	while (n != NULL && IsA(n, RelabelType))
		n = (Node *) ((RelabelType *) n)->arg;
	return n;
}

/* a plain table in the FROM of a query that is nothing else: its range table index, or 0 */
static Index
only_table(Query *q)
{
	RangeTblEntry *rte;
	Index		rti;

	if (q->commandType != CMD_SELECT || q->setOperations != NULL || q->rowMarks != NIL || q->hasAggs || q->hasWindowFuncs || q->hasTargetSRFs || q->hasModifyingCTE ||
		q->cteList != NIL || q->groupClause != NIL || q->groupingSets != NIL || q->havingQual != NULL || q->limitCount != NULL || q->limitOffset != NULL ||
		q->distinctClause != NIL || q->jointree == NULL || list_length(q->jointree->fromlist) != 1 || !IsA(linitial(q->jointree->fromlist), RangeTblRef))
		return 0;
	rti = ((RangeTblRef *) linitial(q->jointree->fromlist))->rtindex;
	rte = rt_fetch(rti, q->rtable);
	return rte->rtekind == RTE_RELATION && (rte->relkind == RELKIND_RELATION || rte->relkind == RELKIND_PARTITIONED_TABLE) && rte->tablesample == NULL ? rti : 0;
}

/* the default equality of the type: the arm's test is the one a hash of the keys answers */
static bool
default_equality(Oid opno, Oid type)
{
	return OidIsValid(opno) && lookup_type_cache(type, TYPECACHE_EQ_OPR)->eq_opr == opno;
}

/* `tbl.col = expr` with expr reading only the level `up` above: the column and the expression */
static bool
key_test(Node *n, Index rti, int up, Var **col, Node **expr, OpExpr **eq)
{
	OpExpr	   *op;
	Node	   *sides[2];

	if (!IsA(n, OpExpr) || list_length(((OpExpr *) n)->args) != 2)
		return false;
	op = (OpExpr *) n;
	sides[0] = strip(linitial(op->args));
	sides[1] = strip(lsecond(op->args));
	for (int i = 0; i < 2; i++)
	{
		Var		   *v = IsA(sides[i], Var) ? (Var *) sides[i] : NULL;

		if (v != NULL && v->varno == rti && v->varlevelsup == 0 && v->varattno > 0 && exprType(sides[1 - i]) == v->vartype &&
			default_equality(op->opno, v->vartype) && only_reaches(sides[1 - i], up) && !contain_volatile_functions(sides[1 - i]) && !checkExprHasSubLink(sides[1 - i]))
		{
			*col = v;
			*expr = sides[1 - i];
			*eq = op;
			return true;
		}
	}
	return false;
}

static void
or_arms(Node *n, List **arms)
{
	if (IsA(n, BoolExpr) && ((BoolExpr *) n)->boolop == OR_EXPR)
		foreach_ptr(Node, a, ((BoolExpr *) n)->args)
			or_arms(a, arms);
	else
		*arms = lappend(*arms, n);
}

/* an arm that is the sub-select of an array of the group of the outer row: `col = ANY(ARRAY(SELECT x FROM g WHERE g.key = outer AND rest))` */
static bool
group_arm(ScalarArrayOpExpr *any, Index rti, Arm *arm)
{
	Node	   *l = strip(linitial(any->args)),
			   *r = strip(lsecond(any->args));
	SubLink    *sl;
	Query	   *g;
	Index		grti;
	TargetEntry *te;
	List	   *rest = NIL;
	Var		   *key = NULL;
	Node	   *outer = NULL;
	OpExpr	   *eq = NULL;

	if (!any->useOr || !IsA(l, Var) || ((Var *) l)->varno != rti || ((Var *) l)->varlevelsup != 0 || !IsA(r, SubLink) || ((SubLink *) r)->subLinkType != ARRAY_SUBLINK)
		return false;
	sl = (SubLink *) r;
	g = castNode(Query, sl->subselect);
	if ((grti = only_table(g)) == 0 || list_length(g->targetList) != 1 || g->hasSubLinks || g->sortClause != NIL)
		return false;
	te = linitial_node(TargetEntry, g->targetList);
	if (!IsA(te->expr, Var) || ((Var *) te->expr)->varno != grti || ((Var *) te->expr)->varlevelsup != 0 || ((Var *) te->expr)->vartype != ((Var *) l)->vartype ||
		!default_equality(any->opno, ((Var *) l)->vartype))
		return false;
	foreach_ptr(Node, c, and_list(g->jointree->quals))
	{
		Var		   *k;
		Node	   *e;
		OpExpr	   *o;

		if (key == NULL && key_test(c, grti, 2, &k, &e, &o))
		{
			key = k;
			outer = e;
			eq = o;
		}
		else if (reads_outside(c) || checkExprHasSubLink(c))
			return false;
		else
			rest = lappend(rest, c);
	}
	if (key == NULL)
		return false;
	arm->group = g;
	arm->group_key = key;
	arm->group_out = (Var *) te->expr;
	arm->group_rest = rest;
	arm->outer = outer;
	arm->eq = eq;
	arm->any = any;
	arm->col = (Var *) l;
	return true;
}

/* the arms of an EXISTS sub-select of the shape above, or false */
static bool
exists_arms(Query *sq, List **arms, List **conds, Index *rti_out)
{
	Index		rti = only_table(sq);
	Node	   *orq = NULL;
	List	   *parts = NIL;

	if (rti == 0 || sq->jointree->quals == NULL)
		return false;
	foreach_ptr(Node, c, and_list(sq->jointree->quals))
	{
		if (orq == NULL && IsA(c, BoolExpr) && ((BoolExpr *) c)->boolop == OR_EXPR)
			orq = c;
		else if (reads_outside(c) || checkExprHasSubLink(c) || contain_volatile_functions(c))
			return false;
		else
			*conds = lappend(*conds, c);
	}
	if (orq == NULL)
		return false;
	or_arms(orq, &parts);
	foreach_ptr(Node, a, parts)
	{
		Arm		   *arm = palloc0(sizeof(Arm));
		Var		   *col;
		Node	   *e;
		OpExpr	   *o;

		a = strip(a);
		if (key_test(a, rti, 1, &col, &e, &o))
		{
			arm->col = col;
			arm->outer = e;
			arm->eq = o;
		}
		else if (!IsA(a, ScalarArrayOpExpr) || !group_arm((ScalarArrayOpExpr *) a, rti, arm))
			return false;
		*arms = lappend(*arms, arm);
	}
	*rti_out = rti;
	return true;
}

static Param *
key_param(Oid type, int32 typmod, Oid collid)
{
	Param	   *p = makeNode(Param);

	p->paramkind = PARAM_SUBLINK;
	p->paramid = 1;
	p->paramtype = type;
	p->paramtypmod = typmod;
	p->paramcollid = collid;
	p->location = -1;
	return p;
}

/* `SELECT col FROM <the table of proto> WHERE conds AND col IS NOT NULL` */
static Query *
key_query(Query *proto, Var *col, List *conds)
{
	Query	   *k = makeNode(Query);
	NullTest   *nn = makeNode(NullTest);
	List	   *quals = NIL;

	nn->arg = (Expr *) copyObject(col);
	nn->nulltesttype = IS_NOT_NULL;
	nn->argisrow = false;
	nn->location = -1;
	k->commandType = CMD_SELECT;
	k->querySource = QSRC_ORIGINAL;
	k->canSetTag = true;
	k->rtable = copyObject(proto->rtable);
	k->rteperminfos = copyObject(proto->rteperminfos);
	quals = list_concat(copyObject(conds), list_make1(nn));
	k->jointree = makeFromExpr(list_make1(copyObject(linitial(proto->jointree->fromlist))), list_length(quals) == 1 ? linitial(quals) : (Node *) makeBoolExpr(AND_EXPR, quals, -1));
	k->targetList = list_make1(makeTargetEntry((Expr *) copyObject(col), 1, "k", false));
	k->hasSubLinks = checkExprHasSubLink((Node *) k->jointree->quals);
	return k;
}

/* `lhs op ANY (sub)`: the sublink a parsed IN makes */
static SubLink *
any_sublink(OpExpr *like, Node *lhs, Query *sub, Var *out)
{
	SubLink    *sl = makeNode(SubLink);
	OpExpr	   *op = copyObject(like);

	op->args = list_make2(lhs, key_param(out->vartype, out->vartypmod, out->varcollid));
	sl->subLinkType = ANY_SUBLINK;
	sl->subLinkId = 0;
	sl->testexpr = (Node *) op;
	sl->operName = list_make1(makeString("="));
	sl->subselect = (Node *) sub;
	sl->location = -1;
	return sl;
}

static Node *
arm_expr(Arm *arm, Query *sq, List *conds)
{
	Node	   *outer = copyObject(arm->outer);
	SubLink    *sl;
	NullTest   *nn = makeNode(NullTest);

	IncrementVarSublevelsUp(outer, arm->group == NULL ? -1 : -2, 1);
	if (arm->group == NULL)
		sl = any_sublink(arm->eq, outer, key_query(sq, arm->col, conds), arm->col);
	else
	{
		/* the keys of the outer row's groups: the table g's key where g.x is among the links' groups */
		OpExpr	   *in_links = makeNode(OpExpr);
		SubLink    *links;
		Query	   *gq;
		List	   *gconds = copyObject(arm->group_rest);

		in_links->opno = arm->any->opno;
		in_links->opfuncid = get_opcode(arm->any->opno);
		in_links->opresulttype = BOOLOID;
		in_links->opretset = false;
		in_links->opcollid = InvalidOid;
		in_links->inputcollid = arm->any->inputcollid;
		in_links->location = -1;
		links = any_sublink(in_links, (Node *) copyObject(arm->group_out), key_query(sq, arm->col, conds), arm->col);
		gconds = lappend(gconds, links);
		gq = key_query(arm->group, arm->group_key, gconds);
		gq->hasSubLinks = true;
		sl = any_sublink(arm->eq, outer, gq, arm->group_key);
	}
	nn->arg = (Expr *) copyObject(outer);
	nn->nulltesttype = IS_NOT_NULL;
	nn->argisrow = false;
	nn->location = -1;
	return (Node *) makeBoolExpr(AND_EXPR, list_make2(nn, sl), -1);
}

typedef struct Split
{
	bool		apply;
	bool		changed;
} Split;

static Node *
split_mutator(Node *n, Split *s)
{
	if (n == NULL)
		return NULL;
	SIFT_SKIP_CONST_LIST(n, n);
	if (IsA(n, Query))
		return n;
	if (IsA(n, SubLink) && ((SubLink *) n)->subLinkType == EXISTS_SUBLINK)
	{
		Query	   *sq = castNode(Query, ((SubLink *) n)->subselect);
		List	   *arms = NIL,
				   *conds = NIL,
				   *args = NIL;
		Index		rti;

		if (!exists_arms(sq, &arms, &conds, &rti))
			return n;
		s->changed = true;
		if (!s->apply)
			return n;
		foreach_ptr(Arm, arm, arms)
			args = lappend(args, arm_expr(arm, sq, conds));
		return list_length(args) == 1 ? linitial(args) : (Node *) makeBoolExpr(OR_EXPR, args, -1);
	}
	return expression_tree_mutator(n, split_mutator, s);
}

static bool
split_level(Query *q, bool apply)
{
	Split		s = {apply, false};

	if (!q->hasSubLinks || q->commandType != CMD_SELECT || q->jointree == NULL || q->jointree->quals == NULL)
		return false;
	q->jointree->quals = split_mutator(q->jointree->quals, &s);
	return s.changed;
}

static bool
split_sublinks(Node *n, Split *s)
{
	if (n == NULL)
		return false;
	SIFT_SKIP_CONST_LIST(n, false);
	if (IsA(n, Query))
	{
		Query	   *q = (Query *) n;

		foreach_node(CommonTableExpr, cte, q->cteList)
			split_sublinks((Node *) cte->ctequery, s);
		foreach_node(RangeTblEntry, rte, q->rtable)
			if (rte->rtekind == RTE_SUBQUERY)
				split_sublinks((Node *) rte->subquery, s);
		if (q->hasSubLinks)
			query_tree_walker(q, split_sublinks, s, QTW_IGNORE_RANGE_TABLE | QTW_IGNORE_CTE_SUBQUERIES);
		s->changed |= split_level(q, s->apply);
		return false;
	}
	return expression_tree_walker(n, split_sublinks, s);
}

/* is there an EXISTS of that shape in the query or the queries inside it? */
bool
sift_agg_exists_or_candidate(Query *q)
{
	Split		s = {false, false};

	split_sublinks((Node *) q, &s);
	return s.changed;
}

void
sift_agg_split_exists_or(Query *q)
{
	Split		s = {true, false};

	split_sublinks((Node *) q, &s);
}
