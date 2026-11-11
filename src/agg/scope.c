/*
 * agg/scope.c - the scope conjunct of a scan (col/scope.h, SiftAggScope)
 *
 * "The rows whose member key has a pair with the key k": a scan of an index that declares the scope reads the scope segments, whose rows are replicated once for each
 * scope key of their member and cut by that key, so the conjunct is a test of the derived leaf "<key_name>#scope" against the keys, which the zones of the units decide
 * (the units of other keys are not read). The units describe the scope relation as of their stamp, so the source leaves out the stored entries of the members whose pairs changed since
 * (sift_col_scope_delta), and reads their rows and those of the write buffers, copied for each pair they have now, as units of memory of the same source: the derived key holds the key
 * of the pair in every row the scan reads. A fact in the units of several keys counts once for IN and EXISTS: the units of its other keys leave its member out (fk NOT IN drop). When the reader may not take the stored rows (sift_scope_read: the setting, the shape of the request, the verdict on the
 * segments) the scan is the one of the entity rows, and the conjunct is the test of the member key against the members the scope relation lists for the keys under the snapshot:
 * the plan that was always exact, as a conjunct of the scan.
 */
/* check-memory: allow-allocs the arrays follow the keys of one scope conjunct and the members the caps of the statement allow, not the rows of the table */
#include "postgres.h"

#include "access/table.h"
#include "utils/lsyscache.h"

#include "core/cmp.h"
#include "col/scope.h"
#include "agg/agg_int.h"
#include "agg/lookcol_int.h"

int
sift_agg_scope_leaf(const SiftSrcLayout *lay, const char *key_name)
{
	const char *path = psprintf("%s#scope", key_name);

	for (int i = 0; i < lay->nleaves; i++)
	{
		const SiftSrcLeaf *lf = &lay->leaf[i];

		if (lf->kind == SCL_SCALAR && lf->parent < 0 && list_length(lf->path) == 1 && strcmp(strVal(linitial(lf->path)), path) == 0)
			return i;
	}
	return -1;
}

static SiftSrcPrune *values_prune(int leaf, const int64 *vals, uint32 n);

/* The rows of the units of the source that the prune entry may keep (their zones and set summaries, as the scan prunes them); vals NULL: of every unit */
static uint64
unit_rows(SiftSrcSource *src, int leaf, const int64 *vals, uint32 n)
{
	SiftSrcUnit u;
	uint64		rows = 0;

	sift_colsrc_begin(src, 0, sift_colsrc_nsb(src), vals ? values_prune(leaf, vals, n) : NULL, vals ? 1 : 0);
	while (sift_colsrc_next(src, &u))
		rows += u.nrow;
	return rows;
}

/*
 * The members the scope relation lists for the keys under the snapshot, ascending: from the leaves of the journal index of the relation (the index holds the member and the scope key,
 * which are all a listing reads, and the join of the planner reads them there), else from the relation.
 */
static int64 *
listed_members(AggExec *ex, Snapshot snapshot, uint32 *n, double *pairs)
{
	const SiftAggScope *sc = ex->spec->scope;
	int64	   *sorted = palloc(Max(sc->nkeys, 1) * sizeof(int64)),
			   *members;
	Relation	rel;
	AttrNumber	member,
				skey;
	Oid			journal = sift_scope_pairs_index(ex->heap, ex->index, sc->key_name, &rel, &member, &skey);
	uint32		nk;

	memcpy(sorted, sc->keys, sc->nkeys * sizeof(int64));
	sift_sort_i64(sorted, sc->nkeys);
	nk = 0;
	for (int i = 0; i < sc->nkeys; i++)
		if (nk == 0 || sorted[nk - 1] != sorted[i])
			sorted[nk++] = sorted[i];
	if (pairs)
		*pairs = Max(rel->rd_rel->reltuples, 0);
	if (!OidIsValid(journal) || !lookcol_members(rel, journal, snapshot, member, skey, sorted, nk, &members, n))
		members = sift_scope_members(ex->heap, ex->index, sc->key_name, snapshot, sc->keys, sc->nkeys, n);
	table_close(rel, NoLock);
	return members;
}

/*
 * Is the entity path cheaper than the units with the patch and the delta? The units read the scope entries of the keys, the patch reads the rows of the members that changed by member key and the
 * delta copies them (and the rows of the write buffers) for each pair. The entity path lists the members of the keys, and reads the units of the entity rows that may hold one of them,
 * which the planner's semi join does the same way: both are counted from the zones and set summaries of the units, as the scan prunes them, and priced by sift_scope_entity_cheaper
 * (col/scope.h), which the planner calls with its estimates. The list the entity path takes is kept.
 */
static bool
patch_dearer(AggExec *ex, Snapshot snapshot)
{
	const SiftAggScope *sc = ex->spec->scope;
	SiftScopeRead *rd = ex->scope_rd;
	MemoryContext cx = AllocSetContextCreate(CurrentMemoryContext, "sift scope price", ALLOCSET_DEFAULT_SIZES);
	SiftSrcSource *units = sift_colsrc_open_scope(ex->heap, ex->index, snapshot, cx, ex->view);
	SiftSrcSource *entity = sift_colsrc_open(ex->heap, ex->index, snapshot, cx, ex->view);
	double		pairs;
	SiftScopeCounts c;
	bool		dearer;

	rd->members = listed_members(ex, snapshot, &rd->nmembers, &pairs);
	c = (SiftScopeCounts) {
		.nkeys      = sc->nkeys,
		.once       = sc->once,
		.entries    = unit_rows(units, sift_agg_scope_leaf(sift_colsrc_source_layout(units), sc->key_name), sc->keys, sc->nkeys),
		.patch_scan_rows = rd->patch_scan,
		.patch_units = rd->patch_units,
		.delta_rows = rd->delta_rows + rd->patch_rows,
		.pairs      = pairs,
		.path_rows  = unit_rows(entity, sc->fk, rd->members, rd->nmembers),
	};
	sift_colsrc_close(entity);
	sift_colsrc_close(units);
	MemoryContextDelete(cx);
	dearer = sift_scope_entity_cheaper(&c);
	if (!dearer)
	{
		pfree(rd->members);
		rd->members = NULL;
	}
	return dearer;
}

/* what the read's patch and delta cost, from the counts it planned */
static double
patch_price(const SiftScopeRead *rd)
{
	SiftScopeCounts c = {.patch_units = rd->patch_units, .patch_scan_rows = rd->patch_scan, .delta_rows = rd->delta_rows + rd->patch_rows};

	return sift_scope_patch_price(&c);
}

/*
 * The leaves of the index the scope read must carry: those the spec reads, and those the leaves after the index's take their values from (the array of a length, the object of
 * its members, the lane of a bound, the scalar of a type), which the rows of the patch and of the delta hold only when asked for
 */
static int *
read_leaves(const SiftAggSpec *s, const SiftSrcLayout *lay, int *n)
{
	int		   *out = palloc((s->nleaves + s->nvleaf * (1 + lay->nleaves)) * sizeof(int));

	*n = 0;
	for (int i = 0; i < s->nleaves; i++)
		out[(*n)++] = s->leaves[i];
	for (int j = 0; j < s->nvleaf; j++)
	{
		const SiftAggVLeaf *v = &s->vleaf[j];

		if (v->bound)
			out[(*n)++] = v->obj + v->bound;
		else
			out[(*n)++] = v->obj;
		for (int i = 0; i < lay->nleaves && sift_aggvleaf_member(v); i++)
			if (lay->leaf[i].parent == v->obj)
				out[(*n)++] = i;
	}
	return out;
}

/*
 * What the join as written costs for `keys` keys of the average size, in instructions: the planner's price at its knots (SiftAggScope), linear between them and flat past the last, which is
 * every key. A first segment goes on below its first knot (a key count the entries of a read give may be a fraction of one).
 */
static double
alt_price(const SiftAggScope *sc, double keys)
{
	int			i = 0;
	double		k = Min(keys, sc->knot_k[sc->nknot - 1]);

	if (sc->nknot == 1)
		return sc->knot_instr[0];
	while (i + 2 < sc->nknot && k > sc->knot_k[i + 1])
		i++;
	return Max(sc->knot_instr[i] + (sc->knot_instr[i + 1] - sc->knot_instr[i]) * (k - sc->knot_k[i]) / (sc->knot_k[i + 1] - sc->knot_k[i]), 0.0);
}

/* the rows of the scope units of the keys: the entries a read of them takes (every unit for every key) */
static uint64
entry_rows(AggExec *ex, Snapshot snapshot)
{
	const SiftAggScope *sc = ex->spec->scope;
	MemoryContext cx = AllocSetContextCreate(CurrentMemoryContext, "sift scope price", ALLOCSET_DEFAULT_SIZES);
	SiftSrcSource *units = sift_colsrc_open_scope(ex->heap, ex->index, snapshot, cx, ex->view);
	uint64		rows = unit_rows(units, sift_agg_scope_leaf(sift_colsrc_source_layout(units), sc->key_name), sc->all ? NULL : sc->keys, sc->nkeys);

	sift_colsrc_close(units);
	MemoryContextDelete(cx);
	return rows;
}

/*
 * Does the join as written (SiftAggScope.alt) cost less than the scope units for the counts c: the units cost their setup, the entries they read and the patch and delta of the changed
 * members (col/scope.h), the join its price for the keys the entries stand for. The join is a scan whose cost grows with the rows the keys leave, and the units' with the entries, so a few keys
 * read the units and many read the join, and the changed members of every key (a patch of the units of the entity segments for each) the join, which does not read them twice.
 */
static bool
join_cheaper(const SiftAggScope *sc, const SiftScopeCounts *c)
{
	return sift_scope_units_price(c) + sc->entry_instr * c->entries > alt_price(sc, sc->knot_rows > 0 ? c->entries / sc->knot_rows : sc->nkeys);
}

/* the reason EXPLAIN gives for the join the node runs */
static const char *
join_cheaper_why(const SiftAggScope *sc)
{
	return sc->all ? "join is cheaper for every key" : psprintf("join is cheaper for %d key%s", sc->nkeys, sc->nkeys == 1 ? "" : "s");
}

/*
 * The node runs the join as written, in the place of its own spec (ex->spec; the EXPLAIN reads the scope from ex->unpacked, which keeps the spec the node was planned with, so that a scan
 * that starts over decides again), when the reader refuses the units (whatever it says: the units cannot answer and the join can) or when it costs less for what this execution finds
 * (join_cheaper). The entries come first, which a few keys cost almost nothing, and where the join is dearer than the setup and the entries of the units alone the reader is not asked; its
 * counts of the changed members and the buffers then make the price whole. A test that wants the units whatever they cost says so (sift.test_scope_patch).
 */
static void
run_join(AggExec *ex, const char *why)
{
	const SiftAggScope *sc = ex->spec->scope;

	ex->scope_why = why;
	SIFT_INVARIANT(sc->alt->nkeys == ex->spec->nkeys && sc->alt->naggs == ex->spec->naggs && sc->alt->nparts == 0, "sift: the join a scope read holds has the groups of the node's own");
	ex->spec = sc->alt;
}

/*
 * Decides how the scan reads the conjunct, before its source is opened (the source of the scope segments is another one); true: the scope segments, and the rows of the entity
 * too when ex->scope_rd->npatch > 0.
 */
bool
scope_open(AggExec *ex, Snapshot snapshot)
{
	const SiftAggScope *sc = ex->spec->scope;
	SiftScopeRead *rd;
	int		   *leaves;
	int			nleaves;
	SiftScopeCounts c;

	ex->scope_rd = NULL;
	ex->scope_why = NULL;
	if (sc == NULL)
		return false;
	leaves = read_leaves(ex->spec, sift_colsrc_layout(ex->index), &nleaves);
	c = (SiftScopeCounts) {.nkeys = sc->nkeys, .once = sc->once};
	if (sc->alt != NULL && !sift_test_scope_patch)
	{
		c.entries = entry_rows(ex, snapshot);
		if (join_cheaper(sc, &c))
		{
			run_join(ex, join_cheaper_why(sc));
			return false;
		}
	}
	rd = ex->scope_rd = sift_scope_read(ex->heap, ex->index, snapshot, sc->key_name, sc->all ? NULL : sc->keys, sc->nkeys, sc->once,
										ex->spec->topn != NULL, leaves, nleaves);
	if (sc->alt != NULL && rd->why != NULL)
	{
		run_join(ex, rd->why);
		return false;
	}
	c.drops = rd->ndrop;
	c.patch_units = rd->patch_units;
	c.patch_scan_rows = rd->patch_scan;
	c.delta_rows = rd->delta_rows + rd->patch_rows;
	if (sc->alt != NULL && !sift_test_scope_patch && join_cheaper(sc, &c))
	{
		run_join(ex, join_cheaper_why(sc));
		return false;
	}
	/* a patch and a delta that cost less than the pricing do not call for it; with the join as written to compare with, join_cheaper has priced them */
	if (rd->why == NULL && sc->alt == NULL && patch_price(rd) > SIFT_SCOPE_PRICING_INSTR && (sc->once || sc->nkeys == 1) && !sift_test_scope_patch && patch_dearer(ex, snapshot))
		rd->why = "patch dearer than the entity path";
	if (rd->why == NULL)
		sift_scope_read_rows(rd);
	ex->scope_why = rd->why;
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

/* the members the units of a key leave out (those the dedupe counts in a lesser key): the conjunct that keeps the others; NULL: none */
static SiftAggPred *
left_out(AggExec *ex, int *total, int fk, const SiftScopeDrop *drop)
{
	if (drop == NULL)
		return NULL;
	return pred_in(ex, total, fk, true, drop->members, drop->n, psprintf("member not counted in a lesser key (%s)", keys_text(drop->members, drop->n)));
}

/* the conjuncts of the units of keys: their key, and the members left out */
static List *
arm_of(AggExec *ex, int *total, int sleaf, int fk, const int64 *keys, uint32 nkeys, const SiftScopeDrop *drop)
{
	List	   *arm = list_make1(pred_in(ex, total, sleaf, false, keys, nkeys, psprintf("%s IN (%s)", ex->lay->leaf[sleaf].name, keys_text(keys, nkeys))));
	SiftAggPred *left = left_out(ex, total, fk, drop);

	return left ? lappend(arm, left) : arm;
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
 * The rows of the keys as conjuncts of the scan: one arm for the keys that have no drop list and one for each key that has, which the scan ORs, or the conjuncts themselves when there is
 * one arm, so that the source can run each on the encoded chunks. The members that changed are not here: the source leaves their stored entries out.
 */
static void
stored_filter(AggExec *ex, int *total, List **add, int sleaf)
{
	const SiftAggScope *sc = ex->spec->scope;
	const SiftScopeRead *rd = ex->scope_rd;
	int64	   *plain = palloc(Max(sc->nkeys, 1) * sizeof(int64));
	uint32		nplain;
	List	   *arms = NIL;

	/* every key: the units are all read, each row has its key */
	ex->scope_preds = 0;
	if (sc->all)
		return;
	nplain = plain_keys(sc, rd, plain);
	if (nplain > 0 || rd->ndrop == 0)
		arms = lappend(arms, arm_of(ex, total, sleaf, sc->fk, plain, nplain, NULL));
	for (int d = 0; d < rd->ndrop; d++)
		arms = lappend(arms, arm_of(ex, total, sleaf, sc->fk, &rd->drop[d].key, 1, &rd->drop[d]));
	if (list_length(arms) == 1)
	{
		*add = list_concat(*add, linitial(arms));
		ex->scope_preds = list_length(linitial(arms));
		return;
	}
	{
		List	   *or_arms = NIL;

		foreach_ptr(List, arm, arms)
			or_arms = lappend(or_arms, conjunction(ex, total, arm, SAP_AND, "scope units of the key"));
		*add = lappend(*add, conjunction(ex, total, or_arms, SAP_OR, "scope units of the keys"));
		ex->scope_preds++;
	}
}

/* the entry that prunes the units of one source of a scope read to the values (a range, and the set when it is short enough for the source's directory) */
static SiftSrcPrune *
values_prune(int leaf, const int64 *vals, uint32 n)
{
	SiftSrcPrune *z = palloc0(sizeof(SiftSrcPrune));
	int64	   *set = palloc(Max(n, 1) * sizeof(int64));
	uint32		k = 0;

	memcpy(set, vals, n * sizeof(int64));
	sift_sort_i64(set, n);
	for (uint32 i = 0; i < n; i++)
		if (k == 0 || set[i] != set[k - 1])
			set[k++] = set[i];
	n = k;
	z->leaf = leaf;
	z->lo = n > 0 ? set[0] : PG_INT64_MAX;
	z->hi = n > 0 ? set[n - 1] : PG_INT64_MIN;
	if (n > 0 && n <= ZONE_SET_CAP)
	{
		z->vals = set;
		z->nvals = n;
	}
	return z;
}

/* The conjunct that keeps the rows of the scope: added to the spec after its own, as the key conjuncts of the joins are (join_filters) */
void
scope_filter(AggExec *ex, int *total, List **add)
{
	const SiftAggScope *sc = ex->spec->scope;
	int64	   *members;
	uint32		n;
	int			leaf = ex->scope_why == NULL ? sift_agg_scope_leaf(ex->lay, sc->key_name) : sc->fk;

	SIFT_INVARIANT(leaf >= 0, "sift: the scope segments of index \"%s\" have no leaf for scope \"%s\"", get_rel_name(ex->spec->indexoid), sc->key_name);
	ex->scope_preds = 0;
	if (ex->scope_why == NULL)
	{
		stored_filter(ex, total, add, leaf);
		return;
	}
	/* a node that holds the join as written has run it (scope_open), whatever the reader said, and the planner gives a read of several keys for each pair no other: only the testing marker, which stands for
	 * the join and has none to run, reaches the error */
	SIFT_INVARIANT(sc->alt == NULL, "sift: a scope read that holds the join as written reads the entity rows");
	if (!sc->once && (sc->all || sc->nkeys > 1))
		ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("a scope read of several keys that counts a row for each pair needs the join: \"%s\"", ex->scope_why),
						errhint("Rewrite the statement over the join to the scope relation, or SET sift.agg_scope to off.")));
	members = ex->scope_rd->members;
	n = ex->scope_rd->nmembers;
	if (members == NULL)
		members = listed_members(ex, ex->snap, &n, NULL);
	ex->scope_members = n;
	ex->scope_preds = 1;
	if (n == 0)
		*add = lappend(*add, pred_node(ex, SAP_FALSE, total, -1, 0, 0, "no member has a pair with the scope keys"));
	else
		*add = lappend(*add, pred_in(ex, total, leaf, false, members, n, psprintf("member of scope %s keys (%s)", sc->key_name, keys_text(sc->keys, sc->nkeys))));
}
