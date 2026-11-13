/*
 * agg/scope_keys.c - the keys of a scope conjunct (agg/plan_scope.c, agg/scope.c)
 *
 * The keys are the constants of the statement, or, for a statement with parameters, the value of an expression of them (a $n, an array $n, a list of them, the output of an InitPlan, a stable
 * function) that the scan evaluates when it starts: a generic plan has the scope read of every value, the reader's verdict is made for the keys of each execution (scope_open).
 */
/* check-memory: allow-allocs the keys are the constants or the parameter of one statement */
#include "postgres.h"

#include "nodes/nodeFuncs.h"
#include "utils/array.h"
#include "utils/lsyscache.h"

#include "agg/agg_int.h"
#include "col/scope.h"
#include "core/cmp.h"

/* the key of a value: the integer, or its code in the map of a text or uuid key */
static int64
key_of(Oid elem, Datum v, SiftKeyMap *km)
{
	int64		code;

	if (km == NULL)
		return elem == INT2OID ? DatumGetInt16(v) : elem == INT4OID ? DatumGetInt32(v) : DatumGetInt64(v);
	(void) sift_keymap_code(km, v, &code);
	return code;
}

/* the keys of a value of an integer type or an array of one: sorted, each once (the NULLs of an array and a NULL value match nothing) */
int64 *
sift_agg_scope_keys(Oid type, Datum value, bool isnull, int *n, SiftKeyMap *km)
{
	Oid			elem = type_is_array(type) ? get_element_type(type) : type;
	Datum	   *vals = &value;
	bool	   *nulls = &isnull;
	int			nvals = 1,
				k = 0;
	int64	   *keys;

	if (type_is_array(type) && !isnull)
	{
		int16		len;
		bool		byval;
		char		align;

		get_typlenbyvalalign(elem, &len, &byval, &align);
		deconstruct_array(DatumGetArrayTypeP(value), elem, len, byval, align, &vals, &nulls, &nvals);
	}
	else if (type_is_array(type))
		nvals = 0;
	keys = palloc(Max(nvals, 1) * sizeof(int64));
	for (int i = 0; i < nvals; i++)
		if (!nulls[i])
			keys[k++] = key_of(elem, vals[i], km);
	sift_sort_i64(keys, k);
	*n = 0;
	for (int i = 0; i < k; i++)
		if (*n == 0 || keys[i] != keys[*n - 1])
			keys[(*n)++] = keys[i];
	return keys;
}

/* The scope conjunct of a statement with parameters: its keys are the value of its expression now, in the memory of the spec (a scan that starts over evaluates them again) */
void
scope_keys_init(AggExec *ex)
{
	SiftAggScope *sc = ex->spec->scope;
	MemoryContext spec_cx = sc == NULL ? NULL : GetMemoryChunkContext(sc);
	MemoryContext old;
	int64	   *keys;
	int			n;
	bool		isnull;
	Datum		value;

	if (sc == NULL)
		return;
	if (OidIsValid(sc->key_type))
		ex->scope_km = sift_scope_keymap(ex->heap, ex->index, sc->key_name);
	if (sc->param == NULL)
		return;
	value = ExecEvalExprSwitchContext(operand_state(ex, sc->param), ex->lutcx, &isnull);
	old = MemoryContextSwitchTo(ex->lutcx->ecxt_per_tuple_memory);
	keys = sift_agg_scope_keys(exprType(sc->param), value, isnull, &n, ex->scope_km);
	MemoryContextSwitchTo(spec_cx);
	if (sc->keys != NULL)
		pfree(sc->keys);
	sc->keys = memcpy(palloc(Max(n, 1) * sizeof(int64)), keys, n * sizeof(int64));
	sc->nkeys = n;
	MemoryContextSwitchTo(old);
	ResetExprContext(ex->lutcx);
}
