/*
 * col/f2/f2_v6_scope_match.c - what the pairs side of a statement has to be for the summary of a scope of two paths to answer it: the pairs of the scope are the distinct (member, key) of the rows of
 * its first path (the pairs relation, by its predicate) and of its second (the groups the key has in a relation, by its predicate, and the members of each group in a via relation, by its predicate).
 * The statement asks for the pairs of one key K as
 *
 *   - the UNION (not UNION ALL, which counts a member of both paths twice) of a sub-select of each path, each stating the predicate of its path and the same key K, or no key at all when the
 *     sub-selects also return the key (a view of the pairs: the statement binds the key above it); or
 *   - SELECT DISTINCT COALESCE(first path member, via member) of the first relation LEFT JOIN the via relation, where the relation holds each row on one path only (a CHECK constraint says so).
 *
 * What is returned stands for the pairs side in the planner of the scope: a sub-select over the first path with its predicate and the key K, the one-path form that the rest of the planner takes.
 */
#include "postgres.h"

#include "access/table.h"
#include "catalog/pg_am_d.h"
#include "catalog/pg_class_d.h"
#include "catalog/pg_inherits.h"
#include "catalog/pg_opfamily_d.h"
#include "commands/defrem.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "optimizer/optimizer.h"
#include "parser/parsetree.h"
#include "rewrite/rewriteManip.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"

#include "col/f2/f2_v6_scope_int.h"
#include "col/f2/f2_v6_scope_match.h"
#include "core/compat.h"

/* The conditions of a query as its clauses: the parser keeps them as one expression, the planner as a list (the query of a sub-select it has planned) */
static List *
quals_of(const Node *quals)
{
	return quals == NULL ? NIL : IsA(quals, List) ? (List *) quals : make_ands_implicit((Expr *) quals);
}

static bool
select_only(const Query *q)
{
	foreach_node(TargetEntry, te, q->targetList)
		if (te->resjunk)
			return false;
	return q->commandType == CMD_SELECT && !q->hasAggs && !q->hasWindowFuncs && !q->hasSubLinks && !q->hasTargetSRFs && q->groupClause == NIL && q->sortClause == NIL && q->limitCount == NULL &&
		q->limitOffset == NULL && q->cteList == NIL && q->rowMarks == NIL && q->havingQual == NULL;
}

Node *
sift_v6_strip_relabel(Node *n)
{
	while (n != NULL && IsA(n, RelabelType))
		n = (Node *) ((RelabelType *) n)->arg;
	return n;
}

/* An integer to an integer, a date, timestamp or timestamptz to its own type, a string to a string, a uuid to a uuid */
bool
sift_v6_key_arg_ok(Oid key_type, Oid typid)
{
	return sift_keymap_same_kind(key_type, typid) || (sift_scope_word_type(key_type) && typid == key_type);
}

/* Is opno the equality of the scope key column: an integer, a date, a timestamp or a timestamptz, or a text or varchar column under a deterministic collation of the operator, or a uuid */
bool
sift_v6_key_equality(Oid key_type, Oid opno, Oid inputcollid)
{
	const Oid	opclass = GetDefaultOpClass(key_type, BTREE_AM_OID);

	if (sift_keymap_type(key_type) && key_type != UUIDOID && OidIsValid(inputcollid) && !get_collation_isdeterministic(inputcollid))
		return false;
	return OidIsValid(opclass) && get_op_opfamily_strategy(opno, get_opclass_family(opclass)) == BTEqualStrategyNumber;
}

static bool
int_operand(const Node *n)
{
	const Oid	t = exprType((Node *) n);

	return (IsA(n, Const) ? !((const Const *) n)->constisnull : IsA(n, Param)) && (t == INT2OID || t == INT4OID || t == INT8OID);
}

static bool
is_var(const Node *n, int varno, AttrNumber attno)
{
	return IsA(n, Var) && ((const Var *) n)->varno == varno && ((const Var *) n)->varattno == attno && ((const Var *) n)->varlevelsup == 0;
}

/* The columns of an arm of the union: the member, or the member and the key in either order; role[i] is 'm' or 'k' */
typedef struct Roles
{
	int			n;
	char		role[2];
} Roles;

static bool
roles_of(const Query *q, int member_rel, AttrNumber member, int key_rel, AttrNumber key, Roles *out)
{
	*out = (Roles) {list_length(q->targetList)};
	if (out->n < 1 || out->n > 2)
		return false;
	for (int i = 0; i < out->n; i++)
	{
		const Node *e = (Node *) ((TargetEntry *) list_nth(q->targetList, i))->expr;

		if (((TargetEntry *) list_nth(q->targetList, i))->resjunk || !(is_var(e, member_rel, member) || is_var(e, key_rel, key)))
			return false;
		out->role[i] = is_var(e, member_rel, member) ? 'm' : 'k';
	}
	return out->n == 1 ? out->role[0] == 'm' : out->role[0] != out->role[1];
}

static bool
returns_key(const Roles *r)
{
	return r->n == 2;
}

static bool
same_roles(const Roles *a, const Roles *b)
{
	return a->n == b->n && memcmp(a->role, b->role, a->n) == 0;
}

static bool
integer_eq(const OpExpr *op)
{
	return list_length(op->args) == 2 && get_op_opfamily_strategy(op->opno, INTEGER_BTREE_FAM_OID) == BTEqualStrategyNumber;
}

/* var(varno, attno) = constant or parameter of an integer type */
static bool
key_clause(const Node *c, int varno, AttrNumber attno, Expr **k)
{
	const OpExpr *op = IsA(c, OpExpr) ? (const OpExpr *) c : NULL;

	if (op == NULL || !integer_eq(op) || !is_var(linitial(op->args), varno, attno) || !int_operand(lsecond(op->args)))
		return false;
	*k = lsecond(op->args);
	return true;
}

/* var(v1, a1) = var(v2, a2), in either order */
static bool
join_clause(const Node *c, int v1, AttrNumber a1, int v2, AttrNumber a2)
{
	const OpExpr *op = IsA(c, OpExpr) ? (const OpExpr *) c : NULL;

	return op != NULL && integer_eq(op) && ((is_var(linitial(op->args), v1, a1) && is_var(lsecond(op->args), v2, a2)) || (is_var(linitial(op->args), v2, a2) && is_var(lsecond(op->args), v1, a1)));
}

static bool
not_null_clause(const Node *c, int varno, AttrNumber attno)
{
	return IsA(c, NullTest) && ((const NullTest *) c)->nulltesttype == IS_NOT_NULL && !((const NullTest *) c)->argisrow && is_var((Node *) ((const NullTest *) c)->arg, varno, attno);
}

/* The clauses are the rows of the btree that gives the path: exactly its predicate (over Vars of varno 1) once it is over varno */
static bool
holds_predicate(List *pred, List *clauses, int varno)
{
	List	   *p = copyObject(pred);

	if (p == NIL || clauses == NIL)
		return p == clauses;
	ChangeVarNodes((Node *) p, 1, varno, 0);
	return predicate_implied_by(p, clauses, false) && predicate_implied_by(clauses, p, false);
}

/* The relations a join tree of inner joins reads, and the conditions it puts on them */
static bool
flatten(Node *n, List **rtis, List **quals)
{
	if (IsA(n, RangeTblRef))
	{
		*rtis = lappend_int(*rtis, ((RangeTblRef *) n)->rtindex);
		return true;
	}
	if (IsA(n, FromExpr))
	{
		const FromExpr *f = (FromExpr *) n;

		if (f->quals != NULL)
			*quals = list_concat(*quals, quals_of(f->quals));
		foreach_ptr(Node, c, f->fromlist)
			if (!flatten(c, rtis, quals))
				return false;
		return true;
	}
	if (IsA(n, JoinExpr) && ((JoinExpr *) n)->jointype == JOIN_INNER && ((JoinExpr *) n)->alias == NULL)
	{
		const JoinExpr *j = (JoinExpr *) n;

		if (j->quals != NULL)
			*quals = list_concat(*quals, quals_of(j->quals));
		return flatten(j->larg, rtis, quals) && flatten(j->rarg, rtis, quals);
	}
	return false;
}

static const RangeTblEntry *
plain_rte(const Query *q, int rtindex, Oid rel)
{
	const RangeTblEntry *rte = rt_fetch(rtindex, q->rtable);

	return rte->rtekind == RTE_RELATION && rte->relid == rel && rte->relkind == RELKIND_RELATION && rte->tablesample == NULL && rte->securityQuals == NIL && !(rte->inh && has_subclass(rel)) ? rte : NULL;
}

static bool
only_var(const Node *c, int varno)
{
	Bitmapset  *v = pull_varnos(NULL, (Node *) c);
	const bool	ok = bms_is_member(varno, v) && bms_num_members(v) == 1;

	bms_free(v);
	return ok;
}

/* The conditions of a query on one relation: the key K, and the others, which must be the predicate of the path */
typedef struct Cond
{
	Node	   *clause;
	Expr	   *key;
	List	   *rest;
} Cond;

static bool
split_conditions(List *quals, int varno, AttrNumber key, AttrNumber member, Cond *out)
{
	*out = (Cond) {0};
	foreach_ptr(Node, c, quals)
	{
		Expr	   *k;

		if (key_clause(c, varno, key, &k))
		{
			if (out->key != NULL)
				return false;
			out->clause = c;
			out->key = k;
		}
		else if (!not_null_clause(c, varno, member))
			out->rest = lappend(out->rest, c);
	}
	return true;
}

/* The first path: the relation by its predicate, the members that are not NULL, the key K when the statement states it */
static bool
first_path(const Query *a, const SiftScopeLeg *leg, Cond *c, Roles *roles)
{
	List	   *quals;

	if (!select_only(a))
		return false;
	quals = quals_of(a->jointree->quals);
	return a->distinctClause == NIL && a->setOperations == NULL && list_length(a->rtable) == 1 && list_length(a->jointree->fromlist) == 1 && IsA(linitial(a->jointree->fromlist), RangeTblRef) &&
		plain_rte(a, 1, leg->rel) != NULL && roles_of(a, 1, leg->member, 1, leg->key, roles) && split_conditions(quals, 1, leg->key, leg->member, c) && holds_predicate(leg->pred, c->rest, 1);
}

/* The second path: the groups of the key K (the relation by its predicate) joined to their members (the via relation by its predicate) */
static bool
second_path(const Query *b, const SiftScopeLeg *leg, Expr **key, Roles *roles)
{
	List	   *rtis = NIL,
			   *quals = NIL,
			   *on_group = NIL,
			   *on_via = NIL;
	int			g,
				m;
	bool		joined = false;
	Cond		c;

	if (!select_only(b) || b->distinctClause != NIL || b->setOperations != NULL || !flatten((Node *) b->jointree, &rtis, &quals) || list_length(rtis) != 2)
		return false;
	g = linitial_int(rtis);
	m = lsecond_int(rtis);
	if (plain_rte(b, g, leg->rel) == NULL || plain_rte(b, m, leg->via_rel) == NULL)
	{
		const int	t = g;

		g = m;
		m = t;
		if (plain_rte(b, g, leg->rel) == NULL || plain_rte(b, m, leg->via_rel) == NULL)
			return false;
	}
	if (!roles_of(b, m, leg->via_member, g, leg->key, roles))
		return false;
	foreach_ptr(Node, q, quals)
	{
		if (join_clause(q, g, leg->member, m, leg->via_join))
			joined = true;
		else if (only_var(q, m))
			on_via = lappend(on_via, q);
		else if (only_var(q, g))
			on_group = lappend(on_group, q);
		else
			return false;
	}
	if (!joined || !split_conditions(on_group, g, leg->key, InvalidAttrNumber, &c) || !holds_predicate(leg->pred, c.rest, g) || !holds_predicate(leg->via_pred, on_via, m))
		return false;
	*key = c.key;
	return true;
}

/* The sub-select that stands for the pairs: the rows of the first path by the key K and its predicate, their members */
static Query *
one_path(const Query *a, const Cond *c)
{
	Query	   *s = copyObject((Query *) a);
	List	   *quals = c->key != NULL ? lcons(c->clause, c->rest) : c->rest;

	s->jointree->quals = quals != NIL ? (Node *) make_ands_explicit(quals) : NULL;
	return s;
}

static Query *
match_union(const Query *q, const SiftScopeLeg legs[2])
{
	const SetOperationStmt *so = (const SetOperationStmt *) q->setOperations;
	const Query *a,
			   *b;
	Cond		ca;
	Expr	   *kb;
	Roles		ra,
				rb;

	if (!IsA(so, SetOperationStmt) || so->op != SETOP_UNION || so->all || !IsA(so->larg, RangeTblRef) || !IsA(so->rarg, RangeTblRef) || q->sortClause != NIL || q->limitCount != NULL ||
		q->limitOffset != NULL || q->cteList != NIL || q->rowMarks != NIL)
		return NULL;
	a = rt_fetch(((RangeTblRef *) so->larg)->rtindex, q->rtable)->subquery;
	b = rt_fetch(((RangeTblRef *) so->rarg)->rtindex, q->rtable)->subquery;
	if (a == NULL || b == NULL || !first_path(a, &legs[0], &ca, &ra) || !second_path(b, &legs[1], &kb, &rb) || !same_roles(&ra, &rb) || (ca.key == NULL) != (kb == NULL) ||
		(ca.key != NULL ? !equal(ca.key, kb) : !returns_key(&ra)))
		return NULL;
	return one_path(a, &ca);
}

/* A relation that holds each row on one path only: a CHECK constraint refutes both members being given */
static bool
paths_exclusive(const SiftScopeLeg legs[2])
{
	Relation	rel = table_open(legs[0].rel, AccessShareLock);
	const TupleConstr *constr = rel->rd_att->constr;
	List	   *both = NIL,
			   *checks = NIL;
	bool		exclusive;

	for (int j = 0; j < 2; j++)
	{
		NullTest   *nt = makeNode(NullTest);
		Oid			type;
		int32		typmod;
		Oid			coll;

		get_atttypetypmodcoll(legs[0].rel, legs[j].member, &type, &typmod, &coll);
		nt->arg = (Expr *) makeVar(1, legs[j].member, type, typmod, coll, 0);
		nt->nulltesttype = IS_NOT_NULL;
		both = lappend(both, nt);
	}
	for (int i = 0; constr != NULL && i < constr->num_check; i++)
		if (constr->check[i].ccvalid && sift_check_enforced(&constr->check[i]))
			checks = list_concat(checks, make_ands_implicit((Expr *) stringToNode(constr->check[i].ccbin)));
	exclusive = predicate_refuted_by(both, checks, false);
	table_close(rel, NoLock);
	return exclusive;
}

/* g LEFT JOIN m ON m.group = g.group AND (the predicate of the via relation) WHERE g.key = K AND (the predicate of both paths), DISTINCT COALESCE(g.member, m.member) */
static Query *
match_left(const Query *q, const SiftScopeLeg legs[2])
{
	const JoinExpr *j = q->jointree != NULL && list_length(q->jointree->fromlist) == 1 && IsA(linitial(q->jointree->fromlist), JoinExpr) ? linitial(q->jointree->fromlist) : NULL;
	const CoalesceExpr *co = list_length(q->targetList) == 1 ? (const CoalesceExpr *) ((TargetEntry *) linitial(q->targetList))->expr : NULL;
	List	   *on_via = NIL,
			   *where = quals_of(q->jointree->quals);
	bool		joined = false;
	int			g,
				m;
	Cond		c;
	Query	   *s;
	Oid			type;
	int32		typmod;
	Oid			coll;

	if (j == NULL || j->jointype != JOIN_LEFT || !IsA(j->larg, RangeTblRef) || !IsA(j->rarg, RangeTblRef) || co == NULL || !IsA(co, CoalesceExpr) || list_length(co->args) != 2 ||
		list_length(q->distinctClause) != 1 || !select_only(q) || q->setOperations != NULL || legs[0].rel != legs[1].rel || legs[0].key != legs[1].key)
		return NULL;
	g = ((RangeTblRef *) j->larg)->rtindex;
	m = ((RangeTblRef *) j->rarg)->rtindex;
	if (plain_rte(q, g, legs[0].rel) == NULL || plain_rte(q, m, legs[1].via_rel) == NULL || !is_var(linitial(co->args), g, legs[0].member) || !is_var(lsecond(co->args), m, legs[1].via_member))
		return NULL;
	foreach_ptr(Node, x, quals_of(j->quals))
	{
		if (join_clause(x, g, legs[1].member, m, legs[1].via_join))
			joined = true;
		else if (only_var(x, m))
			on_via = lappend(on_via, x);
		else
			return NULL;
	}
	if (!joined || !holds_predicate(legs[1].via_pred, on_via, m) || !split_conditions(where, g, legs[0].key, InvalidAttrNumber, &c) || c.key == NULL || !holds_predicate(legs[0].pred, c.rest, g) ||
		!holds_predicate(legs[1].pred, c.rest, g) || !paths_exclusive(legs))
		return NULL;
	get_atttypetypmodcoll(legs[0].rel, legs[0].member, &type, &typmod, &coll);
	s = makeNode(Query);
	s->commandType = CMD_SELECT;
	s->rtable = list_make1(copyObject(rt_fetch(g, q->rtable)));
	s->jointree = makeFromExpr(list_make1(makeNode(RangeTblRef)), NULL);
	((RangeTblRef *) linitial(s->jointree->fromlist))->rtindex = 1;
	s->targetList = list_make1(makeTargetEntry((Expr *) makeVar(1, legs[0].member, type, typmod, coll, 0), 1, NULL, false));
	{
		List	   *quals = copyObject(lcons(c.clause, c.rest));

		ChangeVarNodes((Node *) quals, g, 1, 0);
		s->jointree->quals = (Node *) make_ands_explicit(quals);
	}
	return s;
}

Query *
sift_v6_match_two_paths(const Query *q, const SiftScopeLeg legs[2])
{
	return q->setOperations != NULL ? match_union(q, legs) : match_left(q, legs);
}

bool
sift_v6_semi_joins(const PlannerInfo *root, Index pairs_relid)
{
	const SpecialJoinInfo *sj = list_length(root->join_info_list) == 1 ? linitial(root->join_info_list) : NULL;

	return sj != NULL && sj->jointype == JOIN_SEMI && bms_equal(sj->min_righthand, bms_make_singleton(pairs_relid));
}
