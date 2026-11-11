/*
 * agg/scope_legs.c - the pairs of a scope are the rows of its unique btree that satisfy the btree's predicate (agg/plan_scope.c)
 *
 * The scope units of a scope over a partial unique btree (`WHERE deleted_at IS NULL`) hold the pairs of the rows that satisfy the predicate, so a scope read answers a statement that
 * names the relation only when the rows the statement takes from it are those rows, for the key it binds:
 *  (a) every other condition of the statement on the relation is implied by the predicate (it filters none of the pairs out), and
 *  (b) the conditions together imply the predicate (it takes no row that is not a pair).
 * A plain btree has no predicate: any other condition fails (a). The predicate is the recorded btree's (sift_scope_legs), a copy of the relcache's; the conditions are the copies that are proved.
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
		return (Node *) v;
	}
	return expression_tree_mutator(n, relation_vars, iv);
}

/*
 * NULL: the conditions `rest` of the statement on the scope relation (the item rti, its columns through map) take, for the key the statement binds, the pairs of the scope sp of the index
 * and no other row; else why not.
 */
const char *
scope_pairs_proof(AggCtx *c, const SiftScopeSpecs *specs, const SiftScopeSpec *sp, Index rti, const AttrNumber *map, int nmap, List *rest)
{
	Relation	index = index_open(c->indexoid, AccessShareLock);
	ItemVars	iv = {rti, map, nmap};
	SiftScopeLeg legs[2];
	List	   *mine = NIL;
	int			ord = 0;

	while (specs->s[ord] != sp)
		ord++;
	sift_scope_legs(index, ord, legs);
	index_close(index, NoLock);
	foreach_ptr(Node, q, rest)
	{
		Node	   *m = relation_vars(q, &iv);

		if (!predicate_implied_by(list_make1(m), legs[0].pred, false))
			return psprintf("the scope relation has another condition: %s", expr_text(c, q));
		mine = lappend(mine, m);
	}
	if (!predicate_implied_by(legs[0].pred, mine, false))
		return psprintf("the pairs of the scope are the rows of its unique btree that satisfy %s: the statement does not state it",
						deparse_expression((Node *) make_ands_explicit(legs[0].pred), deparse_context_for(get_rel_name(legs[0].rel), legs[0].rel), false, false));
	return NULL;
}
