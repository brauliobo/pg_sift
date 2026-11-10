/*
 * agg/scope.c - the scope conjunct of a scan (col/scope.h, SiftAggScope)
 *
 * "The rows whose member key has a pair with the key k": a scan of an index that declares the scope reads the scope segments, whose rows are replicated once for each
 * scope key of their member and cut by that key, so the conjunct is a test of the derived leaf "<key_name>#scope" against the keys, which the zones of the units decide
 * (the units of other keys are not read). When the reader may not take the stored rows (sift_scope_why: the setting, the shape of the request, the verdict on the segments)
 * the scan is the one of the entity rows, and the conjunct is the test of the member key against the members the scope relation lists for the keys under the snapshot:
 * the plan that was always exact, as a conjunct of the scan.
 */
#include "postgres.h"

#include "utils/lsyscache.h"

#include "col/scope.h"
#include "agg/agg_int.h"

/* the leaf of the scope key: the one whose path is "<key_name>#scope" (agg/col.c); -1 when the source has none */
static int
scope_leaf(const AggExec *ex, const char *key_name)
{
	const char *path = psprintf("%s#scope", key_name);

	for (int i = 0; i < ex->lay->nleaves; i++)
	{
		const SiftSrcLeaf *lf = &ex->lay->leaf[i];

		if (lf->kind == SCL_SCALAR && lf->parent < 0 && list_length(lf->path) == 1 && strcmp(strVal(linitial(lf->path)), path) == 0)
			return i;
	}
	return -1;
}

/* Decides how the scan reads the conjunct, before its source is opened (the source of the scope segments is another one); true: the scope segments */
bool
scope_open(AggExec *ex)
{
	const SiftAggScope *sc = ex->spec->scope;

	ex->scope_stored = false;
	ex->scope_why = NULL;
	if (sc == NULL)
		return false;
	ex->scope_why = sift_scope_why(ex->index, sc->key_name, sc->nkeys, sc->once, ex->snap);
	ex->scope_stored = ex->scope_why == NULL;
	return ex->scope_stored;
}

static char *
keys_text(const SiftAggScope *sc)
{
	StringInfoData t;

	initStringInfo(&t);
	for (int i = 0; i < sc->nkeys; i++)
		appendStringInfo(&t, "%s" INT64_FORMAT, i ? ", " : "", sc->keys[i]);
	return t.data;
}

/* The conjunct that keeps the rows of the scope: added to the spec after its own, as the key conjuncts of the joins are (join_filters) */
void
scope_filter(AggExec *ex, int *total, List **add)
{
	const SiftAggScope *sc = ex->spec->scope;
	int64	   *members;
	uint32		n;
	int			leaf = ex->scope_stored ? scope_leaf(ex, sc->key_name) : sc->fk;

	SIFT_INVARIANT(leaf >= 0, "sift: the scope segments of index \"%s\" have no leaf for scope \"%s\"", get_rel_name(ex->spec->indexoid), sc->key_name);
	if (ex->scope_stored)
	{
		*add = lappend(*add, pred_in(ex, total, leaf, false, sc->keys, sc->nkeys, psprintf("%s#scope IN (%s)", sc->key_name, keys_text(sc))));
		return;
	}
	members = sift_scope_members(ex->heap, ex->index, sc->key_name, ex->snap, sc->keys, sc->nkeys, &n);
	ex->scope_members = n;
	if (n == 0)
		*add = lappend(*add, pred_node(ex, SAP_FALSE, total, -1, 0, 0, "no member has a pair with the scope keys"));
	else
		*add = lappend(*add, pred_in(ex, total, leaf, false, members, n, psprintf("member of scope %s keys (%s)", sc->key_name, keys_text(sc))));
}
