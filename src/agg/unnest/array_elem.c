/*
 * agg/unnest/array_elem.c - jsonb_array_elements_text over a list of strings the index keeps shredded, as the aggregate scan of an unnest takes it (agg/unnest/jsonb.h): the lengths of the
 * group leaf are the elements of each row, its text leaf their values (a JSON null element is NULL, as the function gives it). A row whose list the leaves cannot hold is read from the heap and
 * taken apart by the function itself.
 */
#include "postgres.h"

#include "catalog/pg_type_d.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "utils/fmgroids.h"

#include "agg/unnest/jsonb.h"
#include "col/am.h"

bool
sift_v6_elems_rte(const RangeTblFunction *rtf, const Expr *v, SiftV6ElemRte *out)
{
	sift_v6_unnest_array_rte(v, out);
	out->fn = SV6E_ELEMS_TEXT;
	return exprType((const Node *) v) == JSONBOID && rtf->funccolcount == 1;
}

int
sift_v6_elems_leaf(const SiftV6Leaves *r, int att, int group)
{
	const SiftColDef *g = &r->schema->cols[group];

	for (int i = r->first[att]; i < r->first[att] + r->nleaves[att]; i++)
	{
		const SiftColDef *d = &r->schema->cols[i];

		if (d->parent == group && d->kind == SIFT_COLK_SCALAR && d->path != NULL && strcmp(d->path, g->path) == 0)
			return sift_v6_leaf_holds(TEXTOID, -1, d) ? i : -1;
	}
	return -1;
}

bool
sift_v6_elems_seed(const SiftV6Leaves *r, SiftV6XElem *e, const char *key, int att, List **names)
{
	if (!r->json[att] || (e->group_leaf = sift_v6_unnest_group_of(r, att, key)) < 0 || (e->col_leaf[0] = sift_v6_elems_leaf(r, att, e->group_leaf)) < 0)
		return false;
	e->col_type[0] = TEXTOID;
	e->ncols = 1;
	*names = list_make1(makeString(pstrdup("value")));
	return true;
}

Oid
sift_v6_elem_fn_oid(SiftV6ElemFn fn)
{
	switch (fn)
	{
		case SV6E_EACH:
			return F_JSONB_EACH;
		case SV6E_EACH_TEXT:
			return F_JSONB_EACH_TEXT;
		case SV6E_ELEMS_TEXT:
			return F_JSONB_ARRAY_ELEMENTS_TEXT;
		default:
			return F_JSONB_TO_RECORDSET;
	}
}
