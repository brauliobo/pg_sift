/*
 * agg/jsonb_order.c - where the scan may raise the error of `(doc ->> 'key')::float8` for a row whose key holds a text that is no number or a container (the leaf of the key cannot hold it: the scan
 * reads it from the exception of its chunk and converts it as PostgreSQL does). PostgreSQL raises only for a row that reaches the cast: one that the conditions of the WHERE it tests before, which its
 * own plan orders by cost and then by position, keep. The scan decides its scan keys on every row first and converts the others on the rows they keep, so it raises for the same rows only when
 * no other condition of the statement can drop a row after the cast has run.
 */
/* check-memory: allow-allocs every list here follows the quals of one statement, not the rows of the table */
#include "postgres.h"

#include "nodes/nodeFuncs.h"
#include "optimizer/cost.h"
#include "optimizer/optimizer.h"

#include "agg/plan/int.h"
#include "agg/expr/pg.h"

static bool
raising_attno(int attno)
{
	return attno >= SIFT_V6_JF8_VATT && attno < SIFT_V6_JF8_VATT + SIFT_V6_JVIRT_SPAN;
}

static bool
raising_col(const SiftV6Col *c)
{
	return c->jleaf > 0 && !c->jtype && c->typid == FLOAT8OID;
}

/* an expression that evaluates its operands in an order, or not at all, so that a cast under it runs for fewer rows than the scan converts */
static bool
short_circuits(Node *n, void *context)
{
	if (n == NULL)
		return false;
	return IsA(n, CaseExpr) || IsA(n, CoalesceExpr) || IsA(n, BoolExpr) || IsA(n, MinMaxExpr) || expression_tree_walker(n, short_circuits, context);
}

static bool
reads_raising_var(Node *n, Bitmapset **leaves)
{
	if (n == NULL)
		return false;
	if (IsA(n, Var) && raising_attno(((const Var *) n)->varattno))
	{
		*leaves = bms_add_member(*leaves, sift_v6_vatt_leaf(((const Var *) n)->varattno));
		return false;
	}
	return expression_tree_walker(n, reads_raising_var, leaves);
}

static Bitmapset *
expr_leaves(const Expr *e)
{
	Bitmapset  *leaves = NULL;

	(void) reads_raising_var((Node *) e, &leaves);
	return leaves;
}

static void
note_col(Bitmapset **leaves, const SiftV6Col *c)
{
	if (raising_col(c))
		*leaves = bms_add_member(*leaves, c->jleaf - 1);
}

/* the leaves the scan converts outside its quals, for the rows it keeps: the keys, the arguments and what is computed from them */
static Bitmapset *
read_leaves(const SiftV6Spec *s, bool *filtered, bool *computed)
{
	Bitmapset  *leaves = NULL;

	for (int i = 0; i < s->nkeys; i++)
		note_col(&leaves, &s->key[i]);
	for (int a = 0; a < s->naggs; a++)
	{
		Bitmapset  *own = NULL;

		note_col(&own, &s->agg[a].arg);
		note_col(&own, &s->agg[a].arg2);
		for (int c = 0; c < s->agg[a].nxcols; c++)
			note_col(&own, &s->agg[a].xcol[c]);
		*filtered |= own != NULL && s->agg[a].fclause != 0;
		leaves = bms_union(leaves, own);
	}
	for (int d = 0; d < s->nderive; d++)
		note_col(&leaves, &s->derive[d].src);
	foreach_ptr(List, item, s->pgexpr)
		for (int i = 1; i < list_length(lsecond(item)); i += SIFT_V6_PGX_INTS)
			if (raising_attno(list_nth_int(lsecond(item), i)))
			{
				leaves = bms_add_member(leaves, sift_v6_vatt_leaf(list_nth_int(lsecond(item), i)));
				*computed |= short_circuits(linitial(item), NULL);
			}
	return leaves;
}

static double
clause_cost(PlannerInfo *root, const RestrictInfo *ri)
{
	QualCost	cost;

	cost_qual_eval_node(&cost, (Node *) ri->clause, root);
	return cost.startup + cost.per_tuple;
}

/* PostgreSQL's own plan of the statement tests every other condition of the WHERE before the clause that casts: it costs less, or as much and is written first */
static bool
tested_before(PlannerInfo *root, const List *clauses, const RestrictInfo *cast)
{
	const double limit = clause_cost(root, cast);
	bool		before = true,
				written = false;

	foreach_node(RestrictInfo, ri, clauses)
	{
		if (ri == cast)
		{
			written = true;
			continue;
		}
		if (!ri->pseudoconstant)
			before &= clause_cost(root, ri) < limit || (clause_cost(root, ri) == limit && !written);
	}
	return before;
}

bool
sift_v6_json_cast_order_ok(V6Plan *p)
{
	const SiftV6Spec *s = &p->spec;
	bool		filtered = false,
				computed = false;
	Bitmapset  *other = read_leaves(s, &filtered, &computed);
	const RestrictInfo *cast = NULL;
	Bitmapset  *cast_leaves = NULL;

	foreach_node(RestrictInfo, ri, p->rel->baserestrictinfo)
	{
		Bitmapset  *leaves = ri->pseudoconstant ? NULL : expr_leaves(sift_v6_plan_json_vars(p, ri->clause, true));

		if (leaves == NULL)
			continue;
		if (cast != NULL)
			return false;
		cast = ri;
		cast_leaves = leaves;
	}
	if (cast == NULL)
		return other == NULL || (!filtered && !computed && s->vqual == NIL);
	return list_length(s->vqual) == 1 && !short_circuits((Node *) cast->clause, NULL) && !filtered && !computed && p->text == NULL &&
		bms_is_subset(other, cast_leaves) && tested_before(p->root, p->rel->baserestrictinfo, cast);
}
