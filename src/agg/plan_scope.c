/*
 * agg/plan_scope.c - the scope conjunct in the WHERE of a scan (col/scope.h, agg/scope.c)
 *
 * Until the planner recognises the joins that mean it (docs/INDEX-RECIPES.md), the test form is the marker sift.in_scope(index, key_name, fk, key or keys): true for the rows
 * whose fk has a pair with the key (one of the keys of the array) in the scope relation the scope key_name of the index declares (its body, in the test flavour, reads that relation under the
 * snapshot, so a statement the scan does not take is exact as written). The scan takes it as the SiftAggScope of the spec when the index is the scanned one, the
 * scope is declared with that fk column and the key is a constant; anything else stays in the quals, which the scan then refuses.
 */
/* check-memory: allow-allocs the keys are the constants of one statement */
#include "postgres.h"

#include "access/genam.h"
#include "catalog/namespace.h"
#include "catalog/pg_type_d.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"

#include "agg/plan_int.h"
#include "col/am.h"
#include "col/scope.h"

static bool
is_marker(Node *q)
{
	FuncExpr   *f;

	if (!IsA(q, FuncExpr))
		return false;
	f = (FuncExpr *) q;
	return list_length(f->args) == 4 && get_func_namespace(f->funcid) == get_namespace_oid("sift", true) &&
		strcmp(get_func_name(f->funcid), "in_scope") == 0;
}

static Const *
arg_const(FuncExpr *f, int i, Oid type)
{
	Node	   *a = strip_relabel(list_nth(f->args, i));

	return IsA(a, Const) && !((Const *) a)->constisnull && ((Const *) a)->consttype == type ? (Const *) a : NULL;
}

/* is key_name a scope of the index that holds its member key in the column of the index attribute att */
static bool
declared_on(Oid indexoid, const char *key_name, int att)
{
	Relation	index = index_open(indexoid, AccessShareLock);
	SiftScopeSpecs *sp = sift_col_is_index(index) ? sift_scope_specs(index) : NULL;
	bool		found = false;

	for (int i = 0; sp != NULL && i < sp->n; i++)
		found |= strcmp(sp->s[i]->key_name, key_name) == 0 &&
			strcmp(sp->s[i]->fk, NameStr(TupleDescAttr(RelationGetDescr(index), att - 1)->attname)) == 0;
	index_close(index, NoLock);
	return found;
}

static SiftAggScope *
marker_scope(AggCtx *c, Node *q)
{
	FuncExpr   *f = (FuncExpr *) q;
	Const	   *idx,
			   *name,
			   *key;
	const SiftSrcLeaf *lf;
	int			leaf;
	Datum	   *elems;
	bool	   *nulls;
	SiftAggScope *sc;

	if (!is_marker(q))
		return NULL;
	idx = arg_const(f, 0, REGCLASSOID);
	name = arg_const(f, 1, TEXTOID);
	key = arg_const(f, 3, INT8OID);
	if (key == NULL)
		key = arg_const(f, 3, INT8ARRAYOID);
	if (idx == NULL || name == NULL || key == NULL || DatumGetObjectId(idx->constvalue) != c->indexoid || (leaf = match_leaf(c, list_nth(f->args, 2))) < 0)
		return NULL;
	lf = leaf_of(c, leaf);
	if (lf->kind != SCL_SCALAR || lf->parent >= 0 || lf->path != NIL || (lf->vtype != SCV_I32 && lf->vtype != SCV_I64) ||
		!declared_on(c->indexoid, TextDatumGetCString(name->constvalue), lf->attno))
		return NULL;
	sc = palloc0(sizeof(SiftAggScope));
	sc->key_name = TextDatumGetCString(name->constvalue);
	sc->fk = leaf;
	if (key->consttype == INT8OID)
	{
		sc->nkeys = 1;
		sc->keys = palloc(sizeof(int64));
		sc->keys[0] = DatumGetInt64(key->constvalue);
	}
	else
	{
		deconstruct_array(DatumGetArrayTypeP(key->constvalue), INT8OID, sizeof(int64), FLOAT8PASSBYVAL, TYPALIGN_DOUBLE, &elems, &nulls, &sc->nkeys);
		sc->keys = palloc(Max(sc->nkeys, 1) * sizeof(int64));
		for (int i = 0; i < sc->nkeys; i++)
		{
			if (nulls[i])
				return NULL;
			sc->keys[i] = DatumGetInt64(elems[i]);
		}
	}
	sc->once = true;
#ifdef SIFT_TESTING
	sc->once = !sift_test_scope_pairs;
#endif
	use_leaf(c, leaf);
	return sc;
}

/* The scope conjunct of the scan, taken out of the quals; NULL (the quals as they were) when there is none the scan reads: one scope, one constant key */
SiftAggScope *
sift_agg_scope_extract(AggCtx *c, List **quals)
{
	if (!sift_agg_scope || c->nparts > 0 || c->search != NULL || !OidIsValid(c->indexoid))
		return NULL;
	foreach_ptr(Node, q, *quals)
	{
		SiftAggScope *sc = marker_scope(c, q);

		if (sc == NULL)
			continue;
		*quals = list_delete_ptr(*quals, q);
		return sc;
	}
	return NULL;
}
