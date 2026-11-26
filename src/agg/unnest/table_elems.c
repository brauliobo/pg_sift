/*
 * agg/unnest/table_elems.c - the elements of an unnest over a sift_table (agg/unnest/jsonb.h): the table keeps the array of a row whole in its cell, and the scan takes it apart from the row with the
 * function of the statement itself (unnest, jsonb_to_recordset, jsonb_array_elements_text, jsonb_array_elements read by members), so the columns of the elements need no leaf: unnest has the one
 * element, a recordset the columns it declares, jsonb_array_elements_text the text of each element, and the members of jsonb_array_elements are added as the statement reads them.
 */
#include "postgres.h"

#include "catalog/pg_type_d.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"

#include "agg/unnest/jsonb.h"

bool
sift_v6_table_elem_type(Oid t)
{
	return t == TEXTOID || t == INT2OID || t == INT4OID || t == INT8OID || t == FLOAT8OID;
}

/* the declared columns of jsonb_to_recordset, of the types the vectors of the elements hold */
static bool
seed_declared(SiftV6XElem *e, const SiftV6ElemRte *rte, List **names)
{
	int			c = 0;

	foreach_node(String, n, rte->names)
	{
		const Oid	t = list_nth_oid(rte->types, c);

		if (!sift_v6_table_elem_type(t))
			return false;
		e->col_leaf[c] = -1;
		e->col_type[c++] = t;
		*names = lappend(*names, makeString(pstrdup(strVal(n))));
	}
	e->ncols = c;
	return true;
}

bool
sift_v6_table_elems_seed(const V6Plan *p, SiftV6XElem *e, const SiftV6ElemRte *rte, List **names)
{
	const Var  *v = (const Var *) rte->base;

	if (!IsA(v, Var) || v->varattno <= 0 || v->varattno > p->sch.natts || (rte->fn != SV6E_UNNEST && v->vartype != JSONBOID))
		return false;
	e->attno = v->varattno;
	e->group_leaf = e->col_leaf[0] = -1;
	e->ncols = 0;
	switch (rte->fn)
	{
		case SV6E_UNNEST:
			e->col_type[0] = rte->elem;
			e->ncols = 1;
			return rte->key == NULL;
		case SV6E_RECORDSET:
			return seed_declared(e, rte, names);
		case SV6E_ELEMS_TEXT:
			e->col_type[0] = TEXTOID;
			e->ncols = 1;
			*names = list_make1(makeString(pstrdup("value")));
			return true;
		case SV6E_MEMBERS:
			return true;
		default:
			return false;
	}
}
