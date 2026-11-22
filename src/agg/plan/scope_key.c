/*
 * agg/plan/scope_key.c - the conditions that bind the key column of a scope item: the equality of a scope key, the column of the table an item column is, and the `key = value`
 * and `key = other.column` conditions of the join.
 */
#include "postgres.h"

#include "catalog/pg_collation.h"
#include "catalog/pg_type.h"
#include "nodes/nodeFuncs.h"
#include "utils/lsyscache.h"
#include "utils/typcache.h"

#include "agg/plan_int.h"

/* the equality of a scope key: the integer one, or the `=` of text (also varchar) or uuid under a deterministic collation, where the bytes of the values decide it, as the codes of the key map do */
bool
key_eq(Oid opno, Oid collation)
{
	if (integer_eq(opno))
		return true;
	return (opno == lookup_type_cache(TEXTOID, TYPECACHE_EQ_OPR)->eq_opr || opno == lookup_type_cache(UUIDOID, TYPECACHE_EQ_OPR)->eq_opr) &&
		(!OidIsValid(collation) || get_collation_isdeterministic(collation));
}

/* the column of the table that column att of the item is (InvalidAttrNumber: none) */
AttrNumber
item_attno(const AttrNumber *map, int nmap, AttrNumber att)
{
	return map == NULL ? att : att > 0 && att < nmap ? map[att] : InvalidAttrNumber;
}

/* n is a plain column of the item rti */
Var *
item_var(Node *n, Index rti)
{
	Node	   *v = strip_relabel(n);

	return IsA(v, Var) && ((Var *) v)->varno == rti && ((Var *) v)->varlevelsup == 0 && ((Var *) v)->varnullingrels == NULL ? (Var *) v : NULL;
}

/*
 * q binds the key column of the item: `key = value`, `value = key` or `key = ANY (array)` (IN (list)). The value, a Const when the statement knows it, else an expression of its
 * parameters; NULL when q is not such a condition.
 */
Node *
key_operand(Index rti, const AttrNumber *map, int nmap, AttrNumber key, Node *q)
{
	Node	   *col,
			   *val;

	if (IsA(q, ScalarArrayOpExpr) && ((ScalarArrayOpExpr *) q)->useOr && key_eq(((ScalarArrayOpExpr *) q)->opno, ((ScalarArrayOpExpr *) q)->inputcollid))
	{
		col = linitial(((ScalarArrayOpExpr *) q)->args);
		val = lsecond(((ScalarArrayOpExpr *) q)->args);
	}
	else if (IsA(q, OpExpr) && list_length(((OpExpr *) q)->args) == 2 && key_eq(((OpExpr *) q)->opno, ((OpExpr *) q)->inputcollid))
	{
		col = linitial(((OpExpr *) q)->args);
		val = lsecond(((OpExpr *) q)->args);
		if (item_var(col, rti) == NULL)
		{
			Node	   *t = col;

			col = val;
			val = t;
		}
	}
	else
		return NULL;
	if (item_var(col, rti) == NULL || item_attno(map, nmap, item_var(col, rti)->varattno) != key)
		return NULL;
	return strip_relabel(val);
}

/* a column of another table than the item rti (not under an outer join) */
static Var *
other_var(Node *n, Index rti)
{
	Node	   *v = strip_relabel(n);

	return IsA(v, Var) && ((Var *) v)->varno != rti && ((Var *) v)->varlevelsup == 0 && ((Var *) v)->varnullingrels == NULL ? (Var *) v : NULL;
}

/* q equates the key column of the item rti to a column of another table (a table of keys): that column; NULL when q is not such a condition */
Var *
scope_key_equality(Index rti, const AttrNumber *map, int nmap, AttrNumber key, Node *q)
{
	OpExpr	   *op = (OpExpr *) q;
	Var		   *mine,
			   *theirs;

	if (!IsA(q, OpExpr) || list_length(op->args) != 2 || !key_eq(op->opno, op->inputcollid))
		return NULL;
	mine = item_var(linitial(op->args), rti);
	theirs = other_var(lsecond(op->args), rti);
	if (mine == NULL || theirs == NULL)
	{
		mine = item_var(lsecond(op->args), rti);
		theirs = other_var(linitial(op->args), rti);
	}
	return mine != NULL && item_attno(map, nmap, mine->varattno) == key ? theirs : NULL;
}
