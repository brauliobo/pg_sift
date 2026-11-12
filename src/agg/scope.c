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

#include <math.h>

#include "common/hashfn.h"
#include "port/pg_bitutils.h"
#include "access/table.h"
#include "utils/lsyscache.h"

#include "core/cmp.h"
#include "core/store.h"
#include "col/am.h"
#include "col/scope.h"
#include "col/seg.h"
#include "agg/agg_int.h"
#include "agg/lookcol_int.h"

static bool test_scope_join;

void
sift_agg_scope_init(void)
{
	DefineCustomBoolVariable("sift.test_scope_join", "Testing: a scope read runs the join the node holds, whatever the units cost.", NULL, &test_scope_join, false,
							 PGC_USERSET, GUC_NOT_IN_SAMPLE, NULL, NULL, NULL);
}

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

/* The units that have a value in the leaf: those of the scope the leaf is the derived key of (another scope's units and the entity units hold NULL there) */
static SiftSrcPrune *
not_null_prune(int leaf)
{
	SiftSrcPrune *z = palloc0(sizeof(SiftSrcPrune));

	z->leaf = leaf;
	z->nulltest = SSN_NOTNULL;
	return z;
}

/*
 * The rows of the units of the source that the prune entry may keep (their zones and set summaries, as the scan prunes them); NULL: of every unit. The source says which units it keeps without
 * loading them (sift_colsrc_kept); one that cannot is read unit by unit.
 */
static uint64
unit_rows(SiftSrcSource *src, SiftSrcPrune *prune, uint64 *units)
{
	SiftSrcUnit u;
	uint32		nsb = sift_colsrc_nsb(src);
	uint64	   *kept = palloc0(Max((nsb + 63) / 64, 1) * sizeof(uint64));
	uint64		rows = 0;

	*units = 0;
	if (sift_colsrc_kept(src, prune, prune ? 1 : 0, nsb, kept, &rows, false))
	{
		for (uint32 w = 0; w < (nsb + 63) / 64; w++)
			*units += pg_popcount64(kept[w]);
		return rows;
	}
	sift_colsrc_begin(src, 0, nsb, prune, prune ? 1 : 0);
	while (sift_colsrc_next(src, &u))
	{
		rows += u.nrow;
		(*units)++;
	}
	return rows;
}

/*
 * The members the scope relation lists for the keys under the snapshot, ascending: from the leaves of the journal index of the relation (the index holds the member and the scope key,
 * which are all a listing reads, and the join of the planner reads them there), else from the relation.
 */
static int64 *
listed_members(AggExec *ex, Snapshot snapshot, uint32 *n)
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
	if (!OidIsValid(journal) || ex->scope_km != NULL || !lookcol_members(rel, journal, snapshot, member, skey, sorted, nk, &members, n))
		members = sift_scope_members(ex->heap, ex->index, sc->key_name, ex->scope_km, snapshot, sc->keys, sc->nkeys, n);
	table_close(rel, NoLock);
	return members;
}

/* the rows of the relations the pairs of the scope are listed from, by the way they are listed */
static void
pair_counts(AggExec *ex, SiftScopeCounts *c)
{
	double		scanned;
	double		rows = sift_scope_relations_rows(ex->heap, ex->index, ex->spec->scope->key_name, &scanned);

	c->pairs = rows - scanned;
	c->scan_pairs = scanned;
}

/* the entries of the scope segments, the rows a scan of every key reads */
static uint64
scope_entries(AggExec *ex)
{
	SiftDirSnapshot *dir = palloc(sizeof(SiftDirSnapshot));
	uint64		rows = 0;

	sift_dir_read(ex->index, dir);
	for (int i = 0; i < dir->nsegs; i++)
		if (dir->segs[i].kind == SIFT_SEG_IMMUTABLE && sift_seg_is_scope(&dir->segs[i]))
			rows += dir->segs[i].ndocs;
	pfree(dir);
	return rows;
}

/*
 * A read of units whose changed members' entries leave them (SCOPE_CHANGED_SPREAD) reads those units row by row: of the keys, a changed member leaves the share exp(-SCOPE_CHANGED_SPREAD * changed / keys)
 * of the units without one (the fit of the entries read, 12.8% of them with 4 changed keys, 44% with 18, 92% with 45; 300,000 events, 5,000 members, 50 keys).
 */
#define SCOPE_CHANGED_SPREAD	1.8

/* what the journal's expectation of the patch may err by: the join is run for it when the units cost more than this times its price */
#define SCOPE_ESTIMATE_MARGIN	1.1

/* The keys the entries of a read stand for, as the join as written counts them: the entries of one key of the average size, which the planner's knots give */
static double
alt_keys(const SiftAggScope *sc, uint64 entries)
{
	return sc->knot_rows > 0 ? entries / sc->knot_rows : sc->nkeys;
}

/*
 * The units and entries a read of the keys has, which only the directory of the index changes (a promotion, a merge, a build: its version) and the keys it reads: a read of the same keys
 * of the same directory asks the segments no more than once (a statement that runs again, the read of a prepared one, the reads of a workload of a few keys). A directory that is read again
 * costs the version of a pinned page.
 */
#define UNIT_MEMO_N 8
static struct
{
	Oid			index;
	RelFileNumber file;
	uint32		version;
	uint32		keys;
	uint64		units;
	uint64		entries;
} unit_memo[UNIT_MEMO_N];
static int	unit_memo_next;

static uint32
unit_memo_keys(const SiftAggScope *sc)
{
	uint32		h = hash_bytes((const unsigned char *) sc->key_name, strlen(sc->key_name));

	return sc->all ? ~h : h ^ hash_bytes((const unsigned char *) sc->keys, sc->nkeys * sizeof(int64));
}

/* the entries and units of the scope units of the keys, from the memo or the segments */
static void
unit_entries(AggExec *ex, Snapshot snapshot, SiftScopeCounts *c)
{
	const SiftAggScope *sc = ex->spec->scope;
	Buffer		dirbuf = sift_dir_pin(ex->index);
	uint32		version = sift_dir_version_peek(dirbuf),
				keys = unit_memo_keys(sc);
	MemoryContext cx;
	SiftSrcSource *units;
	int			leaf;

	ReleaseBuffer(dirbuf);
	for (int i = 0; i < UNIT_MEMO_N; i++)
		if (unit_memo[i].index == RelationGetRelid(ex->index) && unit_memo[i].file == ex->index->rd_locator.relNumber && unit_memo[i].version == version && unit_memo[i].keys == keys)
		{
			sift_catalog_memo_hits++;
			c->units = unit_memo[i].units;
			c->entries = unit_memo[i].entries;
			return;
		}
	cx = AllocSetContextCreate(CurrentMemoryContext, "sift scope price", ALLOCSET_DEFAULT_SIZES);
	units = sift_colsrc_open_scope(ex->heap, ex->index, snapshot, cx, ex->view);
	leaf = sift_agg_scope_leaf(sift_colsrc_source_layout(units), sc->key_name);
	c->entries = unit_rows(units, sc->all ? not_null_prune(leaf) : values_prune(leaf, sc->keys, sc->nkeys), &c->units);
	sift_colsrc_close(units);
	MemoryContextDelete(cx);
	unit_memo[unit_memo_next++ % UNIT_MEMO_N] = (typeof(unit_memo[0])) {RelationGetRelid(ex->index), ex->index->rd_locator.relNumber, version, keys, c->units, c->entries};
}

/*
 * The counts of the scope units of the keys (col/scope.h): the units and their entries as the scan prunes them, and the pairs of the relation a read of several keys counted once lists whole. With several keys
 * counted once none of the units is answered from their pre-aggregates.
 */
static SiftScopeCounts
unit_counts(AggExec *ex, Snapshot snapshot)
{
	const SiftAggScope *sc = ex->spec->scope;
	SiftScopeCounts c = {.nkeys = sc->nkeys,.once = sc->once,.entry_instr = sc->entry_instr};

	unit_entries(ex, snapshot, &c);
	c.preagg = sc->alt == NULL || (sc->once && sc->nkeys > 1) ? 0.0 : 1.0 - sc->rowwise;
	pair_counts(ex, &c);
	if (sc->once && sc->nkeys > 1)
	{
		c.key_pairs = (c.pairs + c.scan_pairs) * c.entries / Max(scope_entries(ex), 1);	/* the pairs of the keys, as many of the relation's as the keys have of the entries: the reader counts them */
	}
	return c;
}

/* ... and what the reader found: the lists of the dedupe, the pairs it listed, the changed members, the patch and the delta; the units a changed member's entries leave are read row by row */
static void
read_counts(const SiftAggScope *sc, SiftScopeCounts *c, const SiftScopeRead *rd)
{
	c->drops = rd->ndrop;
	c->key_pairs = rd->pairs;
	c->changed = rd->nexcept;
	c->patch_units = rd->patch_units;
	c->patch_scan_rows = rd->patch_scan;
	c->delta_rows = rd->delta_rows + rd->patch_rows;
	c->preagg *= exp(-SCOPE_CHANGED_SPREAD * rd->nexcept / Max(alt_keys(sc, c->entries), 1.0));
}

/* what the read's patch and delta cost, from the counts it planned */
static double
patch_price(const SiftScopeRead *rd)
{
	SiftScopeCounts c = {.patch_units = rd->patch_units, .patch_scan_rows = rd->patch_scan, .delta_rows = rd->delta_rows + rd->patch_rows};

	return sift_scope_patch_price(&c);
}

/* ... and with the changed members, at the least a member costs (the pairs of the relations are not known yet) */
static double
patch_extra(const SiftScopeRead *rd)
{
	return patch_price(rd) + SIFT_SCOPE_MEMBER_INSTR * (double) rd->nexcept;
}

/*
 * Is the entity path cheaper than the units with the changed members, the patch and the delta? The units read the scope entries of the keys, the changed members are listed and the patch reads the rows of the
 * members that changed by member key, which the delta copies (with the rows of the write buffers) for each pair; changed and rd: the members the journal lists, and what the read planned of the patch (NULL
 * before it is planned). The entity path lists the members of the keys from the relations of the scope and reads the units of the entity rows, as many as the layout leaves it: both are priced by
 * sift_scope_entity_cheaper (col/scope.h), which the planner calls with its estimates. The members are not listed to price it (the listing is the dearest part of the entity path, and the units read are then
 * those of the zones of the member leaf, which prune a layout that clusters it only): all the units and rows count.
 */
static bool
entity_cheaper(AggExec *ex, Snapshot snapshot, uint64 changed, const SiftScopeRead *rd)
{
	const SiftAggScope *sc = ex->spec->scope;
	SiftScopeCounts c = unit_counts(ex, snapshot);
	MemoryContext cx = AllocSetContextCreate(CurrentMemoryContext, "sift scope price", ALLOCSET_DEFAULT_SIZES);
	SiftSrcSource *entity = sift_colsrc_open(ex->heap, ex->index, snapshot, cx, ex->view);
	bool		cheaper;

	if (rd != NULL)
		read_counts(sc, &c, rd);
	c.changed = changed;
	c.path_rows = unit_rows(entity, NULL, &c.path_units);
	cheaper = sift_scope_entity_cheaper(&c);
	sift_colsrc_close(entity);
	MemoryContextDelete(cx);
	return cheaper;
}

/* the extra the changed members, the patch and the delta cost on top of the clean read of the units */
static bool
patch_dearer(AggExec *ex, Snapshot snapshot)
{
	const SiftScopeRead *rd = ex->scope_rd;
	SiftScopeCounts c = {0};

	/* the entity path costs at least what listing the pairs does; changed members and a patch under half of it leave the units, which the entries of the keys bound, below the rest in every measured read, and the units are not counted */
	pair_counts(ex, &c);
	c.changed = rd->nexcept;
	if (sift_scope_members_price(&c) + patch_price(rd) <= sift_scope_entity_price(&c) / 2)
		return false;
	return entity_cheaper(ex, snapshot, rd->nexcept, rd);
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

/*
 * Does the join as written (SiftAggScope.alt) cost less than the scope units for the counts c: the units cost what col/scope.h says of the counts (the entries and units they read, the listing of the pairs, the
 * changed members and the patch and delta of them), the join its price for the keys the entries stand for. The join is a scan whose cost grows with the rows the keys leave, and the units' with the entries, so a few keys
 * read the units and many read the join, and the changed members of every key (a patch of the units of the entity segments for each) the join, which does not read them twice.
 */
static bool
join_cheaper(const SiftAggScope *sc, const SiftScopeCounts *c)
{
	return sift_scope_units_price(c) > alt_price(sc, alt_keys(sc, c->entries));
}

/*
 * the changed members past which the join costs less: the room the clean read leaves under the join, less the journal, over what asking for a member costs; none when the pairs of the relations read whole
 * (which the read takes for the changed members once they cost more) cost less than the room: the count is then no reason to refuse
 */
static uint32
changed_limit(const SiftAggScope *sc, const SiftScopeCounts *c)
{
	double		room = alt_price(sc, alt_keys(sc, c->entries)) - sift_scope_units_price(c) - SIFT_SCOPE_CHANGED_INSTR,
				leaf = sift_scope_members_leaf(c);

	if (leaf > 0 && leaf <= room)
		return PG_UINT32_MAX;
	return (uint32) Min(Max(room, 0.0) / sift_scope_member_instr(c), (double) PG_UINT32_MAX);
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
 * (join_cheaper). The counts of the units come first, which a few keys cost almost nothing, and where the join is dearer than what they price alone the reader is not asked; it is told how many changed
 * members the join leaves it (changed_limit) and refuses the read before it lists more; its counts of the changed members, the patch and the buffers then make the price whole. A test that wants the units
 * whatever they cost says so (sift.test_scope_patch).
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
	uint32		max_changed = PG_UINT32_MAX;
	bool		priced = sc != NULL && sc->alt != NULL && !sift_test_scope_patch;

	ex->scope_rd = NULL;
	ex->scope_why = NULL;
	if (sc == NULL)
		return false;
	if (sc->alt != NULL && test_scope_join)
	{
		run_join(ex, "sift.test_scope_join");
		return false;
	}
	leaves = read_leaves(ex->spec, sift_colsrc_layout(ex->index), &nleaves);
	if (priced)
	{
		c = unit_counts(ex, snapshot);
		if (join_cheaper(sc, &c))
		{
			run_join(ex, join_cheaper_why(sc));
			return false;
		}
		max_changed = changed_limit(sc, &c);
	}
	rd = ex->scope_rd = sift_scope_read(ex->heap, ex->index, snapshot, sc->key_name, ex->scope_km, sc->all ? NULL : sc->keys, sc->nkeys, sc->once,
										ex->spec->topn != NULL, leaves, nleaves, max_changed, priced);
	if (priced && rd->why == NULL && rd->stage != NULL)
	{
		/* the journal alone says what the changed members cost: a join that costs less by more than the error of that is run without asking for their pairs */
		SiftScopeCounts e = c;

		read_counts(sc, &e, rd);
		if (sift_scope_units_price(&e) > alt_price(sc, alt_keys(sc, e.entries)) * SCOPE_ESTIMATE_MARGIN)
		{
			run_join(ex, join_cheaper_why(sc));
			return false;
		}
	}
	sift_scope_read_pairs(rd);
	if (sc->alt != NULL && rd->why != NULL)
	{
		run_join(ex, rd->why);
		return false;
	}
	if (priced)
		read_counts(sc, &c, rd);
	if (priced && join_cheaper(sc, &c))
	{
		run_join(ex, join_cheaper_why(sc));
		return false;
	}
	/* a patch and a delta that cost less than the pricing do not call for it; with the join as written to compare with, join_cheaper has priced them */
	if (rd->why == NULL && sc->alt == NULL && patch_extra(rd) > SIFT_SCOPE_PRICING_INSTR && (sc->once || sc->nkeys == 1) && !sift_test_scope_patch && patch_dearer(ex, snapshot))
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

	/* every key: the units are all read, each row has its key, but the units of another scope of the index have none (the zones of the key's leaf are NULL there): a key list leaves them out by its keys */
	if (sc->all)
	{
		*add = lappend(*add, pred_node(ex, SAP_NULL, total, sleaf, 1, 0, psprintf("%s IS NOT NULL", ex->lay->leaf[sleaf].name)));
		ex->scope_preds = 1;
		return;
	}
	ex->scope_preds = 0;
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
	members = listed_members(ex, ex->snap, &n);
	ex->scope_members = n;
	ex->scope_preds = 1;
	if (n == 0)
		*add = lappend(*add, pred_node(ex, SAP_FALSE, total, -1, 0, 0, "no member has a pair with the scope keys"));
	else
		*add = lappend(*add, pred_in(ex, total, leaf, false, members, n, psprintf("member of scope %s keys (%s)", sc->key_name, keys_text(sc->keys, sc->nkeys))));
}
