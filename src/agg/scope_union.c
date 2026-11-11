/*
 * agg/scope_union.c - the union of the two paths of a scope is the pairs relation of the scope (agg/plan_scope.c)
 *
 * The pairs of a scope of two paths (docs/COL-SCOPE.md) are the distinct (member, key) of its direct path, the rows of its first relation, and of its joined path, the rows of its second relation
 * each joined by its group column to the rows of its via relation. A statement that asks for the pairs writes the union of the two:
 *
 *   SELECT d.member [, d.key] FROM pairs d WHERE d.key = K AND <predicate of the btree of pairs>
 *   UNION [ALL]
 *   SELECT m.member [, g.key] FROM groups g JOIN via m ON m.group = g.group WHERE g.key = K AND <predicate of groups> AND <predicate of via>
 *
 * as the select of an IN or an EXISTS, as an item of a join, through a view or an inlined function: PostgreSQL does not pull the union up (UNION ALL it makes an append relation, whose
 * selects are the same), so the item of the FROM list keeps its query as written (rte->subquery), and its columns are the ones the first select projects by position.
 *
 * The item is the pairs relation of the scope, read through the column map of its outputs, when:
 *  (a) it is a union of two selects, one of the direct path and one of the joined path, each over plain tables only (no view, no row-level security, no child tables), with nothing but a
 *      projection of columns: the member and, if it is there, the key, the same in both;
 *  (b) each relation of an arm is read for the pairs of its btree: its conditions are the predicate of the btree and the key binding, and none other (agg/scope_legs.c), the joined path by one
 *      equality of the group column of its second relation to that of the via relation;
 *  (c) the arms bind the key the same, or the statement binds it above the union (the arms may carry a copy PostgreSQL pushed down, that the query as written does not have).
 * A union that does not hold is no pair set of the scope, and the reason says which of them fails.
 */
#include "postgres.h"

#include "catalog/pg_class.h"
#include "catalog/pg_inherits.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "optimizer/optimizer.h"
#include "optimizer/pathnode.h"
#include "parser/parse_relation.h"
#include "parser/parsetree.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"

#include "agg/plan_int.h"
#include "col/scope.h"

#define ROLE_MEMBER	1
#define ROLE_KEY	2

/* the selects of a set operation, over a tree of operations that are all UNION (only_union) */
typedef struct Leaves
{
	List	   *select;
	bool		all;
	bool		only_union;
} Leaves;

/* a select of the union: its tables (range table indexes, all plain tables when checked) and its conditions as a list, with the parameters of the statement in */
typedef struct Arm
{
	Query	   *q;
	List	   *tables;
	List	   *quals;
} Arm;

static void
set_leaves(Query *q, Node *n, Leaves *lv)
{
	if (IsA(n, RangeTblRef))
		lv->select = lappend(lv->select, rt_fetch(((RangeTblRef *) n)->rtindex, q->rtable)->subquery);
	else
	{
		SetOperationStmt *so = castNode(SetOperationStmt, n);

		lv->all = so->all;
		lv->only_union &= so->op == SETOP_UNION;
		set_leaves(q, so->larg, lv);
		set_leaves(q, so->rarg, lv);
	}
}

/* the tables and the conditions of the jointree of a select as the rewriter left it: inner joins of tables only */
static bool
arm_tree(PlannerInfo *root, Node *n, Arm *a)
{
	if (IsA(n, RangeTblRef))
		a->tables = lappend_int(a->tables, ((RangeTblRef *) n)->rtindex);
	else if (IsA(n, FromExpr))
	{
		foreach_ptr(Node, x, ((FromExpr *) n)->fromlist)
			if (!arm_tree(root, x, a))
				return false;
		a->quals = list_concat(a->quals, make_ands_implicit((Expr *) eval_const_expressions(root, ((FromExpr *) n)->quals)));
	}
	else if (IsA(n, JoinExpr) && ((JoinExpr *) n)->jointype == JOIN_INNER)
	{
		JoinExpr   *j = (JoinExpr *) n;

		if (!arm_tree(root, j->larg, a) || !arm_tree(root, j->rarg, a))
			return false;
		a->quals = list_concat(a->quals, make_ands_implicit((Expr *) eval_const_expressions(root, j->quals)));
	}
	else
		return false;
	return true;
}

/* the table of an arm that is not a plain table the union may be read through, else NULL */
static const char *
arm_tables_why(const Arm *a)
{
	foreach_int(rti, a->tables)
	{
		RangeTblEntry *e = rt_fetch(rti, a->q->rtable);

		if (e->rtekind != RTE_RELATION || e->relkind != RELKIND_RELATION || e->tablesample != NULL || has_subclass(e->relid))
			return "an arm of the union reads something other than a table without child tables";
		if (e->securityQuals != NIL)
			return "row-level security applies to the scope relation";
	}
	return NULL;
}

static Oid
arm_relid(const Arm *a, int i)
{
	return rt_fetch(list_nth_int(a->tables, i), a->q->rtable)->relid;
}

/* does the arm read exactly the table r0 and, if r1 is valid, the table r1 */
static bool
arm_reads(const Arm *a, Oid r0, Oid r1)
{
	if (!OidIsValid(r1))
		return list_length(a->tables) == 1 && arm_relid(a, 0) == r0;
	return list_length(a->tables) == 2 && ((arm_relid(a, 0) == r0 && arm_relid(a, 1) == r1) || (arm_relid(a, 0) == r1 && arm_relid(a, 1) == r0));
}

/* is the table relid one of the relations of the scope whose legs are legs (n of them) */
static bool
leg_relation(const SiftScopeLeg *legs, int n, Oid relid)
{
	return legs[0].rel == relid || (n == 2 && (legs[1].rel == relid || legs[1].via_rel == relid));
}

/* the roles of the columns the select projects (ROLE_MEMBER, ROLE_KEY): how many, or -1 for a column that is none of them or a repeated one */
static int
arm_roles(const Query *q, Index mrti, AttrNumber member, Index krti, AttrNumber key, int role[2])
{
	int			n = 0;

	foreach_node(TargetEntry, te, q->targetList)
	{
		const Var  *v = (const Var *) te->expr;

		if (n == 2 || !IsA(te->expr, Var) || v->varlevelsup != 0)
			return -1;
		role[n] = v->varno == mrti && v->varattno == member ? ROLE_MEMBER : v->varno == krti && v->varattno == key ? ROLE_KEY : 0;
		if (role[n] == 0 || (n == 1 && role[1] == role[0]))
			return -1;
		n++;
	}
	return n == 2 || (n == 1 && role[0] == ROLE_MEMBER) ? n : -1;
}

static const char *
bind_key(Node **key, Node *val)
{
	if (*key != NULL && !equal(*key, val))
		return "the scope key is bound more than once";
	*key = val;
	return NULL;
}

static const char *
arm_why(Oid relid, const char *why)
{
	return why == NULL ? NULL : psprintf("an arm of the union on %s: %s", get_rel_name(relid), why);
}

/* the direct arm: the pairs of the leg, its key binding in *key */
static const char *
direct_arm(const SiftScopeLeg *leg, const Arm *a, Node **key, int role[2], int *nrole)
{
	Index		d = linitial_int(a->tables);
	List	   *rest = NIL;
	const char *why;

	foreach_ptr(Node, q, a->quals)
	{
		Node	   *val = key_operand(d, NULL, 0, leg->key, q);
	
		if (val == NULL)
			rest = lappend(rest, q);
		else if ((why = bind_key(key, val)) != NULL)
			return why;
	}
	if ((*nrole = arm_roles(a->q, d, leg->member, d, leg->key, role)) < 0)
		return "the union projects other columns than the member and the key of the pairs";
	return arm_why(leg->rel, scope_arm_proof(leg->rel, leg->pred, leg->member, leg->key, d, rest));
}

/* q equates the column a of the table x to the column b of the table y */
static bool
equates(Node *q, Index x, AttrNumber a, Index y, AttrNumber b)
{
	OpExpr	   *op = (OpExpr *) q;
	Var		   *l,
			   *r;

	if (!IsA(q, OpExpr) || list_length(op->args) != 2 || !integer_eq(op->opno))
		return false;
	l = item_var(linitial(op->args), x);
	r = item_var(lsecond(op->args), y);
	if (l == NULL || r == NULL)
	{
		l = item_var(lsecond(op->args), x);
		r = item_var(linitial(op->args), y);
	}
	return l != NULL && r != NULL && l->varattno == a && r->varattno == b;
}

/* the joined arm: the groups of the leg joined by their group column to the members of their via relation; its key binding in *key */
static const char *
joined_arm(const SiftScopeLeg *leg, const Arm *a, Node **key, int role[2], int *nrole)
{
	Node	   *join = NULL;
	Index		g = 0,
				m = 0;
	List	   *grest = NIL,
			   *mrest = NIL;
	const char *why;

	for (int i = 0; i < 2 && join == NULL; i++)
	{
		Index		x = list_nth_int(a->tables, i),
					y = list_nth_int(a->tables, 1 - i);

		if (arm_relid(a, i) != leg->rel || arm_relid(a, 1 - i) != leg->via_rel)
			continue;
		foreach_ptr(Node, q, a->quals)
			if (join == NULL && equates(q, y, leg->via_join, x, leg->member))
			{
				join = q;
				g = x;
				m = y;
			}
	}
	if (join == NULL)
		return "the select of the joined path does not join its groups to the relation of their member by the group column";
	foreach_ptr(Node, q, a->quals)
	{
		Relids		v = pull_varnos(NULL, q);
		Node	   *val;

		if (q == join)
			continue;
		if (bms_is_member(g, v) && bms_is_member(m, v))
			return "a condition of the select of the joined path reads both its tables";
		if (bms_is_member(m, v))
			mrest = lappend(mrest, q);
		else if ((val = key_operand(g, NULL, 0, leg->key, q)) == NULL)
			grest = lappend(grest, q);
		else if ((why = bind_key(key, val)) != NULL)
			return why;
	}
	if ((*nrole = arm_roles(a->q, m, leg->via_member, g, leg->key, role)) < 0)
		return "the union projects other columns than the member and the key of the pairs";
	if ((why = arm_why(leg->rel, scope_arm_proof(leg->rel, leg->pred, leg->member, leg->key, g, grest))) != NULL)
		return why;
	return arm_why(leg->via_rel, scope_arm_proof(leg->via_rel, leg->via_pred, leg->via_join, leg->via_member, m, mrest));
}

/* the arm that reads the first path of the scope of the legs and the one that reads the second (0 or 1: the direct one); -1: the arms read other relations, or the scope has one path */
static int
paths_of(const Arm arm[2], const SiftScopeLeg *legs, int n)
{
	if (n != 2)
		return -1;
	if (arm_reads(&arm[0], legs[0].rel, InvalidOid) && arm_reads(&arm[1], legs[1].rel, legs[1].via_rel))
		return 0;
	return arm_reads(&arm[1], legs[0].rel, InvalidOid) && arm_reads(&arm[0], legs[1].rel, legs[1].via_rel) ? 1 : -1;
}

/* the first scope (of n, legs[i] its nlegs[i]) whose two paths the arms read, with its direct arm in *direct; -1: none */
static int
scope_of_arms(const Arm arm[2], SiftScopeLeg (*legs)[2], const int *nlegs, int n, int *direct)
{
	for (int i = 0; i < n; i++)
		if ((*direct = paths_of(arm, legs[i], nlegs[i])) >= 0)
			return i;
	return -1;
}

/*
 * The union of the item q as the pairs relation of the scope of two paths its arms read, with the reason when it is not (ScopeItem.why). The first scope of the index whose relations an arm
 * reads is the item's sp: none, and the item is none of the index's.
 */
ScopeItem
scope_union_item(AggCtx *c, const SiftScopeSpecs *specs, Query *q)
{
	ScopeItem	it = {0};
	Leaves		lv = {.only_union = true};
	Relation	index = index_open(c->indexoid, AccessShareLock);
	SiftScopeLeg legs[SIFT_SCOPE_MAX][2];
	int			nlegs[SIFT_SCOPE_MAX],
				ord = -1,
				mine = -1,
				direct = -1,
				role[2][2],
				nrole[2];
	Arm			arm[2] = {{0}};
	Node	   *key[2] = {NULL, NULL};
	const char *why = NULL;

	set_leaves(q, q->setOperations, &lv);
	for (int i = 0; i < specs->n; i++)
		nlegs[i] = sift_scope_legs(index, i, legs[i]);
	index_close(index, NoLock);
	foreach_ptr(Query, leaf, lv.select)
		foreach_node(RangeTblEntry, e, leaf->rtable)
			for (int i = 0; i < specs->n && mine < 0; i++)
				if (e->rtekind == RTE_RELATION && leg_relation(legs[i], nlegs[i], e->relid))
					mine = i;
	if (mine < 0)
		return it;
	it.sp = specs->s[mine];
	if (!lv.only_union || list_length(lv.select) != 2)
		why = "the sub-select is not a union of two selects";
	for (int i = 0; why == NULL && i < 2; i++)
	{
		arm[i].q = list_nth(lv.select, i);
		if (!plain_select(arm[i].q) || !arm_tree(c->root, (Node *) arm[i].q->jointree, &arm[i]))
			why = "an arm of the union is not a plain select: it has DISTINCT, GROUP BY, LIMIT, an aggregate, a sub-select or a join that is not an inner join";
		else
			why = arm_tables_why(&arm[i]);
	}
	if (why == NULL && (ord = scope_of_arms(arm, legs, nlegs, specs->n, &direct)) < 0)
		why = "the union is not the two paths of a scope: one select reads only the pairs relation, the other its joined relation and the relation that lists the member of each group";
	if (why == NULL && (why = direct_arm(&legs[ord][0], &arm[direct], &key[0], role[0], &nrole[0])) == NULL)
		why = joined_arm(&legs[ord][1], &arm[1 - direct], &key[1], role[1], &nrole[1]);
	if (why == NULL && (nrole[0] != nrole[1] || memcmp(role[0], role[1], nrole[0] * sizeof(int)) != 0))
		why = "the two selects of the union project different columns";
	if (why == NULL && key[0] != NULL && key[1] != NULL && !equal(key[0], key[1]))
		why = "the arms of the union bind different keys";
	it.why = why;
	if (why != NULL)
		return it;
	it.sp = specs->s[ord];
	it.relid = legs[ord][0].rel;
	it.is_union = true;
	it.all = lv.all;
	it.member_only = nrole[0] == 1;
	it.nmap = nrole[0] + 1;
	it.map = palloc0(it.nmap * sizeof(AttrNumber));
	for (int i = 0; i < nrole[0]; i++)
		it.map[i + 1] = role[0][i] == ROLE_MEMBER ? legs[ord][0].member : legs[ord][0].key;
	it.key = key[0] != NULL ? key[0] : key[1];
	it.key_both = key[0] != NULL && key[1] != NULL;
	for (int i = 0; i < 2; i++)
		foreach_int(rti, arm[i].tables)
		{
			RangeTblEntry *e = rt_fetch(rti, arm[i].q->rtable);

			it.rtes = lappend(it.rtes, e);
			it.perms = lappend(it.perms, getRTEPermissionInfo(arm[i].q->rteperminfos, e));
		}
	return it;
}

/*
 * The plan of the scope read has no scan of the arms, and the tables of a sub-select that is not in the plan are not in the range table the executor checks the privileges on: they join the
 * range table of the query (the way PostgreSQL adds the tables of an inheritance child), with the checks the arms carry, which are those of the statement, or of the owner of a view it reads through.
 */
void
scope_union_perms(PlannerInfo *root, const ScopeItem *it)
{
	Query	   *parse = root->parse;
	ListCell   *le,
			   *lp;

	forboth(le, it->rtes, lp, it->perms)
	{
		RangeTblEntry *e;

		if (list_member(parse->rteperminfos, lfirst(lp)))
			continue;
		parse->rteperminfos = lappend(parse->rteperminfos, copyObject(lfirst(lp)));
		e = copyObject((RangeTblEntry *) lfirst(le));
		e->inh = false;
		e->perminfoindex = list_length(parse->rteperminfos);
		parse->rtable = lappend(parse->rtable, e);
		expand_planner_arrays(root, 1);
		root->simple_rte_array[list_length(parse->rtable)] = e;
	}
}
