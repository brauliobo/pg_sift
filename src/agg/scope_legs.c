/*
 * agg/scope_legs.c - the pairs of a scope are the rows of its unique btree that satisfy the btree's predicate (agg/plan_scope.c)
 *
 * The scope units of a scope over a partial unique btree (`WHERE deleted_at IS NULL`) hold the pairs of the rows that satisfy the predicate, so a scope read answers a statement that
 * names the relation only when the rows the statement takes from it are those rows, for the key it binds:
 *  (a) every other condition of the statement on the relation is implied by the predicate (it filters none of the pairs out), and
 *  (b) the conditions together imply the predicate (it takes no row that is not a pair).
 * A plain btree has no predicate: any other condition fails (a). The predicate is the recorded btree's (sift_scope_legs), a copy of the relcache's; the conditions are the copies that are proved.
 * The rows of a path whose column (the member, the key, the column that joins) is NULL are no pairs, as a join drops them: a condition that says the column is not NULL narrows nothing.
 * The same proof holds for each relation of the arms of a union of the two paths of a scope (agg/scope_union.c).
 */
#include "postgres.h"

#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "optimizer/optimizer.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"
#include "utils/ruleutils.h"

#include "agg/plan_int.h"
#include "col/scope.h"

typedef struct ItemVars
{
	Index		rti;
	const AttrNumber *map;
	int			nmap;
} ItemVars;

/* the Vars of the item rti as columns of the relation (varno 1, the numbering of an index predicate), through the column map of a view over it */
static Node *
relation_vars(Node *n, ItemVars *iv)
{
	if (n == NULL)
		return NULL;
	if (IsA(n, Var) && ((Var *) n)->varno == iv->rti && ((Var *) n)->varlevelsup == 0)
	{
		Var		   *v = copyObject((Var *) n);

		v->varno = 1;
		v->varattno = item_attno(iv->map, iv->nmap, v->varattno);
		v->varnosyn = 1;
		v->varattnosyn = v->varattno;
		return (Node *) v;
	}
	return expression_tree_mutator(n, relation_vars, iv);
}

/* `column IS NOT NULL` of the relation */
static Node *
not_null(Oid relid, AttrNumber att)
{
	NullTest   *nt = makeNode(NullTest);
	Oid			type,
				coll;
	int32		typmod;

	get_atttypetypmodcoll(relid, att, &type, &typmod, &coll);
	nt->arg = (Expr *) makeVar(1, att, type, typmod, coll, 0);
	nt->nulltesttype = IS_NOT_NULL;
	nt->location = -1;
	return (Node *) nt;
}

/*
 * NULL: the conditions of the statement quals, mine as conditions over the Vars of varno 1 of the relation relid, take, of the rows that have the columns a and b, those that satisfy the predicate
 * pred and no other; else why not. A condition is said as the statement says it, c being its context (NULL: an arm of a union, which has a range table of its own: the condition as the relation's).
 */
static const char *
leg_proof(AggCtx *c, Oid relid, List *pred, AttrNumber a, AttrNumber b, List *quals, List *mine)
{
	List	   *narrowing = list_concat_copy(pred, list_make2(not_null(relid, a), not_null(relid, b)));
	ListCell   *lq,
			   *lm;

	forboth(lq, quals, lm, mine)
		if (!predicate_implied_by(list_make1(lfirst(lm)), narrowing, false))
			return psprintf("the scope relation has another condition: %s",
							c ? expr_text(c, lfirst(lq)) : deparse_expression(lfirst(lm), deparse_context_for(get_rel_name(relid), relid), false, false));
	if (!predicate_implied_by(pred, mine, false))
		return psprintf("the pairs of the scope are the rows of its unique btree that satisfy %s: the statement does not state it",
						deparse_expression((Node *) make_ands_explicit(pred), deparse_context_for(get_rel_name(relid), relid), false, false));
	return NULL;
}

/* the conditions quals of the item rti (its columns through map) as conditions over the columns of the relation */
static List *
relation_conditions(List *quals, Index rti, const AttrNumber *map, int nmap)
{
	ItemVars	iv = {rti, map, nmap};
	List	   *mine = NIL;

	foreach_ptr(Node, q, quals)
		mine = lappend(mine, relation_vars(q, &iv));
	return mine;
}

/*
 * NULL: the conditions `rest` of the statement on the scope relation (the item rti, its columns through map) take, for the key the statement binds, the pairs of the scope sp of the index
 * and no other row; else why not.
 */
const char *
scope_pairs_proof(AggCtx *c, const SiftScopeSpecs *specs, const SiftScopeSpec *sp, Index rti, const AttrNumber *map, int nmap, List *rest)
{
	Relation	index = index_open(c->indexoid, AccessShareLock);
	SiftScopeLeg legs[2];
	int			ord = 0;

	while (specs->s[ord] != sp)
		ord++;
	sift_scope_legs(index, ord, legs);
	index_close(index, NoLock);
	return leg_proof(c, legs[0].rel, legs[0].pred, legs[0].member, legs[0].key, rest, relation_conditions(rest, rti, map, nmap));
}

const char *
scope_arm_proof(Oid relid, List *pred, AttrNumber a, AttrNumber b, Index rti, List *quals)
{
	return leg_proof(NULL, relid, pred, a, b, quals, relation_conditions(quals, rti, NULL, 0));
}
