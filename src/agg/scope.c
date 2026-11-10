/*
 * agg/scope.c - the scope conjunct of a scan (col/scope.h, SiftAggScope)
 *
 * "The rows whose member key has a pair with the key k": a scan of an index that declares the scope reads the scope segments, whose rows are replicated once for each
 * scope key of their member and cut by that key, so the conjunct is a test of the derived leaf "<key_name>#scope" against the keys, which the zones of the units decide
 * (the units of other keys are not read). The units describe the scope relation as of their stamp, so the scan leaves out the members whose pairs changed since and those that
 * have a row in a write buffer (fk NOT IN except), and reads the entity rows of those of them that have a pair now (scope IS NULL AND fk IN patch: the derived key is NULL in
 * the rows of the entity segments and the write buffers) as one stream, one disjunct of the same conjunct. A fact in the units of several keys counts once for IN and EXISTS:
 * the units of its other keys leave its member out. When the reader may not take the stored rows (sift_scope_read: the setting, the shape of the request, the verdict on the
 * segments) the scan is the one of the entity rows, and the conjunct is the test of the member key against the members the scope relation lists for the keys under the snapshot:
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

/*
 * Decides how the scan reads the conjunct, before its source is opened (the source of the scope segments is another one); true: the scope segments, and the rows of the entity
 * too when ex->scope_rd->npatch > 0.
 */
bool
scope_open(AggExec *ex, Snapshot snapshot)
{
	const SiftAggScope *sc = ex->spec->scope;

	ex->scope_rd = NULL;
	ex->scope_why = NULL;
	if (sc == NULL)
		return false;
	ex->scope_rd = sift_scope_read(ex->heap, ex->index, snapshot, sc->key_name, sc->keys, sc->nkeys, sc->once, ex->spec->topn != NULL);
	ex->scope_why = ex->scope_rd->why;
	return ex->scope_why == NULL;
}

static char *
keys_text(const int64 *keys, uint32 n)
{
	StringInfoData t;

	initStringInfo(&t);
	for (uint32 i = 0; i < n && i < 8; i++)
		appendStringInfo(&t, "%s" INT64_FORMAT, i ? ", " : "", keys[i]);
	if (n > 8)
		appendStringInfo(&t, ", ... (%u)", n);
	return t.data;
}

/* a conjunction of the preds (a single one is itself) */
static SiftAggPred *
conjunction(AggExec *ex, int *total, List *preds, SiftAggPredKind kind, const char *desc)
{
	SiftAggPred *p;

	if (list_length(preds) == 1)
		return linitial(preds);
	p = pred_node(ex, kind, total, -1, 0, list_length(preds), desc);
	for (int i = 0; i < p->nchild; i++)
		p->child[i] = list_nth(preds, i);
	return p;
}

/* the conjuncts of the units of keys: their key, and the members left out (the changed and write buffer members, and those of the drop) */
static List *
arm_of(AggExec *ex, int *total, int sleaf, int fk, const int64 *keys, uint32 nkeys, const SiftScopeRead *rd, const SiftScopeDrop *drop)
{
	List	   *arm = list_make1(pred_in(ex, total, sleaf, false, keys, nkeys, psprintf("%s IN (%s)", ex->lay->leaf[sleaf].name, keys_text(keys, nkeys))));
	uint32		nleft = rd->nexcept + (drop ? drop->n : 0);

	if (nleft > 0)
	{
		int64	   *left = palloc(nleft * sizeof(int64));

		memcpy(left, rd->except, rd->nexcept * sizeof(int64));
		if (drop)
			memcpy(left + rd->nexcept, drop->members, drop->n * sizeof(int64));
		arm = lappend(arm, pred_in(ex, total, fk, true, left, nleft, psprintf("member not changed since the scope stamp (%s)", keys_text(left, nleft))));
	}
	return arm;
}

/* the distinct keys that have no drop list */
static uint32
plain_keys(const SiftAggScope *sc, const SiftScopeRead *rd, int64 *out)
{
	uint32		n = 0;

	for (int i = 0; i < sc->nkeys; i++)
	{
		bool		skip = false;

		for (int d = 0; d < rd->ndrop; d++)
			skip |= rd->drop[d].key == sc->keys[i];
		for (uint32 j = 0; j < n; j++)
			skip |= out[j] == sc->keys[i];
		if (!skip)
			out[n++] = sc->keys[i];
	}
	return n;
}

/*
 * The stored rows of the keys as conjuncts of the scan: one arm for the keys that have no drop list and one for each key that has, which the scan ORs (and the patch is one more
 * arm), or the conjuncts themselves when there is one arm and nothing to patch, so that the source can run each on the encoded chunks.
 */
static void
stored_filter(AggExec *ex, int *total, List **add, int sleaf)
{
	const SiftAggScope *sc = ex->spec->scope;
	const SiftScopeRead *rd = ex->scope_rd;
	int64	   *plain = palloc(Max(sc->nkeys, 1) * sizeof(int64));
	uint32		nplain = plain_keys(sc, rd, plain);
	List	   *arms = NIL;

	if (nplain > 0 || rd->ndrop == 0)
		arms = lappend(arms, arm_of(ex, total, sleaf, sc->fk, plain, nplain, rd, NULL));
	for (int d = 0; d < rd->ndrop; d++)
		arms = lappend(arms, arm_of(ex, total, sleaf, sc->fk, &rd->drop[d].key, 1, rd, &rd->drop[d]));
	if (rd->npatch == 0 && list_length(arms) == 1)
	{
		*add = list_concat(*add, linitial(arms));
		ex->scope_preds += list_length(linitial(arms));
		return;
	}
	{
		List	   *or_arms = NIL;
		SiftAggPred *root;

		foreach_ptr(List, arm, arms)
			or_arms = lappend(or_arms, conjunction(ex, total, arm, SAP_AND, "scope units of the key, members not changed"));
		if (rd->npatch > 0)
		{
			List	   *patch = list_make2(pred_node(ex, SAP_NULL, total, sleaf, 0, 0, "entity rows (no scope key)"),
										   pred_in(ex, total, sc->fk, false, rd->patch, rd->npatch,
												   psprintf("member that changed, with a pair now (%s)", keys_text(rd->patch, rd->npatch))));

			or_arms = lappend(or_arms, conjunction(ex, total, patch, SAP_AND, "entity rows of the changed members"));
		}
		root = conjunction(ex, total, or_arms, SAP_OR, "scope units or the changed members' entity rows");
		*add = lappend(*add, root);
		ex->scope_preds++;
	}
}

/* The conjunct that keeps the rows of the scope: added to the spec after its own, as the key conjuncts of the joins are (join_filters) */
void
scope_filter(AggExec *ex, int *total, List **add)
{
	const SiftAggScope *sc = ex->spec->scope;
	int64	   *members;
	uint32		n;
	int			leaf = ex->scope_why == NULL ? scope_leaf(ex, sc->key_name) : sc->fk;

	SIFT_INVARIANT(leaf >= 0, "sift: the scope segments of index \"%s\" have no leaf for scope \"%s\"", get_rel_name(ex->spec->indexoid), sc->key_name);
	ex->scope_preds = 0;
	if (ex->scope_why == NULL)
	{
		stored_filter(ex, total, add, leaf);
		return;
	}
	/* one row for each pair of a member in several keys is what only the join of the scope relation gives: the planner keeps it as the fallback of such a scope */
	if (!sc->once && sc->nkeys > 1)
		ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("a scope read of several keys that counts a row for each pair needs the join: \"%s\"", ex->scope_why)));
	members = sift_scope_members(ex->heap, ex->index, sc->key_name, ex->snap, sc->keys, sc->nkeys, &n);
	ex->scope_members = n;
	ex->scope_preds = 1;
	if (n == 0)
		*add = lappend(*add, pred_node(ex, SAP_FALSE, total, -1, 0, 0, "no member has a pair with the scope keys"));
	else
		*add = lappend(*add, pred_in(ex, total, leaf, false, members, n, psprintf("member of scope %s keys (%s)", sc->key_name, keys_text(sc->keys, sc->nkeys))));
}
