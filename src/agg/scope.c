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

#include "common/int.h"
#include "utils/lsyscache.h"

#include "col/scope.h"
#include "agg/agg_int.h"

static int
cmp_key(const void *a, const void *b)
{
	return pg_cmp_s64(*(const int64 *) a, *(const int64 *) b);
}

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
	ex->scope_dedupe = false;
	ex->scope_why = NULL;
	if (sc == NULL)
		return false;
	/* the units of several keys have no order across the keys: a top-N of a fact counted once reads the entity rows, as it always did */
	ex->scope_why = ex->spec->topn != NULL && sc->once && sc->nkeys > 1 ? "top-n over several keys" : sift_scope_why(ex->index, sc->key_name, sc->nkeys, sc->once, ex->snap);
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

/*
 * A fact counted once for several keys is in the units of each of the keys its member has: the entry of the unit of the first of them is the one that counts. The conjunct is
 * "scope key = k0, or scope key = k and the member is one whose first key is k" for each other key k: the members of each key come from one pass of the scope relation, and
 * the rows of the first key need no test of the member (all of its members have it first).
 */
static SiftAggPred *
first_key_pred(AggExec *ex, int *total, const SiftAggScope *sc, int scope_leaf_id)
{
	/* check-memory: allow the keys of one statement's array argument, not rows */
	int64	   *keys = palloc(sc->nkeys * sizeof(int64));
	int64	  **set;
	uint32	   *count;
	int			nk = 0,
				nb = 0;
	SiftAggPred *root;

	memcpy(keys, sc->keys, sc->nkeys * sizeof(int64));
	qsort(keys, sc->nkeys, sizeof(int64), cmp_key);
	for (int i = 0; i < sc->nkeys; i++)
		if (nk == 0 || keys[i] != keys[nk - 1])
			keys[nk++] = keys[i];
	set = sift_scope_first_members(ex->heap, ex->index, sc->key_name, ex->snap, keys, nk, &count);
	for (int k = 0; k < nk; k++)
		nb += k == 0 || count[k] > 0;
	root = pred_node(ex, SAP_OR, total, -1, 0, nb, "the first of the member's scope keys");
	for (int k = 0, b = 0; k < nk; k++)
	{
		SiftAggPred *key;

		if (k > 0 && count[k] == 0)
			continue;
		key = pred_in(ex, total, scope_leaf_id, false, &keys[k], 1, psprintf("%s#scope = " INT64_FORMAT, sc->key_name, keys[k]));
		if (k == 0)
			root->child[b++] = key;
		else
		{
			SiftAggPred *both = pred_node(ex, SAP_AND, total, -1, 0, 2, "the scope key, and the member's first key");

			both->child[0] = key;
			both->child[1] = pred_in(ex, total, sc->fk, false, set[k], count[k], psprintf("member whose first key is " INT64_FORMAT, keys[k]));
			root->child[b++] = both;
		}
	}
	return root;
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
		if (sc->once && sc->nkeys > 1)
		{
			*add = lappend(*add, first_key_pred(ex, total, sc, leaf));
			ex->scope_dedupe = true;
		}
		return;
	}
	members = sift_scope_members(ex->heap, ex->index, sc->key_name, ex->snap, sc->keys, sc->nkeys, &n);
	ex->scope_members = n;
	if (n == 0)
		*add = lappend(*add, pred_node(ex, SAP_FALSE, total, -1, 0, 0, "no member has a pair with the scope keys"));
	else
		*add = lappend(*add, pred_in(ex, total, leaf, false, members, n, psprintf("member of scope %s keys (%s)", sc->key_name, keys_text(sc))));
}
