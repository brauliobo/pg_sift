/*
 * agg/scope.c - the scope conjunct of a scan (col/scope.h, SiftAggScope)
 *
 * "The rows whose member key has a pair with the key k": the scan reads the entity rows of the members the scope relation lists for the keys under the snapshot, and the runs of the scope
 * sidecar (col/side.h) of the key stand for the units whose members' pairs have not changed. When the join as written costs less for what this execution finds, the node runs it in the place of
 * its own spec. A read of several keys that counts a row for each pair, or of every key, is the join.
 */
/* check-memory: allow-allocs the arrays follow the keys of one scope conjunct and the members the caps of the statement allow, not the rows of the table */
#include "postgres.h"

#include <math.h>

#include "catalog/namespace.h"
#include "catalog/pg_collation_d.h"
#include "catalog/pg_operator_d.h"
#include "catalog/pg_type_d.h"
#include "common/hashfn.h"
#include "nodes/makefuncs.h"
#include "utils/array.h"
#include "utils/fmgroids.h"
#include "utils/uuid.h"
#include "port/pg_bitutils.h"
#include "access/table.h"
#include "utils/lsyscache.h"

#include "core/ring.h"
#include "core/cmp.h"
#include "core/store.h"
#include "lib/stringinfo.h"
#include "col/am.h"
#include "col/conn.h"
#include "col/scope.h"
#include "col/side.h"
#include "col/seg.h"
#include "agg/agg_int.h"
#include "agg/cost/model.h"
#include "agg/lookcol_int.h"
#include "agg/topn.h"

#ifdef SIFT_TESTING
static bool test_scope_join;
static bool test_scope_entity;
static bool test_scope_no_side;
#define TEST_SCOPE_JOIN test_scope_join
#define TEST_SCOPE_ENTITY test_scope_entity
#define TEST_SCOPE_NO_SIDE test_scope_no_side

void
sift_agg_scope_init(void)
{
	DefineCustomBoolVariable("sift.test_scope_join", "Testing: a scope read runs the join the node holds, whatever the units cost.", NULL, &test_scope_join, false,
							 PGC_USERSET, GUC_NOT_IN_SAMPLE, NULL, NULL, NULL);
	DefineCustomBoolVariable("sift.test_scope_entity", "Testing: a scope read scans the entity rows of the members the scope lists, whatever the join costs.", NULL, &test_scope_entity, false,
							 PGC_USERSET, GUC_NOT_IN_SAMPLE, NULL, NULL, NULL);
	DefineCustomBoolVariable("sift.test_scope_no_side", "Testing: a scan of the entity rows of a scope read does not take the runs of the scope sidecar.", NULL, &test_scope_no_side, false,
							 PGC_USERSET, GUC_NOT_IN_SAMPLE, NULL, NULL, NULL);
}
#else
#define TEST_SCOPE_JOIN false
#define TEST_SCOPE_ENTITY false
#define TEST_SCOPE_NO_SIDE false
#endif

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

/* does a group key of the spec read the derived leaf of the scope key: the key itself, as a number or as the value of the leaf */
bool
sift_agg_scope_groups_by_key(const SiftAggSpec *s, const SiftSrcLayout *lay)
{
	int			leaf = sift_agg_scope_leaf(lay, s->scope->key_name);
	sift_i128	mul,
				add;

	for (int j = 0; j < s->nkeys; j++)
		if ((s->key[j].kind == SAK_LEAF && s->key[j].leaf == leaf) ||
			(s->key[j].kind == SAK_NUM && sift_aggnum_linear(&s->key[j].num, &mul, &add) == leaf))
			return true;
	return false;
}

/* Does the read count a row for each pair of several keys, or of the key it groups by: only the join counts them, so the node holds the one the first pass priced. A scalar key the execution evaluates is one key. */
bool
sift_agg_scope_counts_pairs(const SiftAggScope *sc, bool key_read)
{
	return !sc->once && (sc->all || sc->nkeys > 1 || key_read);
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

	ex->scope_mm = sift_scope_member_map(ex->heap, ex->index, sc->key_name);
	memcpy(sorted, sc->keys, sc->nkeys * sizeof(int64));
	sift_sort_i64(sorted, sc->nkeys);
	nk = 0;
	for (int i = 0; i < sc->nkeys; i++)
		if (nk == 0 || sorted[nk - 1] != sorted[i])
			sorted[nk++] = sorted[i];
	if (!OidIsValid(journal) || ex->scope_km != NULL || !lookcol_members(rel, journal, snapshot, member, skey, sorted, nk, &members, n))
		members = sift_scope_members(ex->heap, ex->index, sc->key_name, ex->scope_km, ex->scope_mm, snapshot, sc->keys, sc->nkeys, n);
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

/* The keys the entries of a read stand for, as the join as written counts them: the entries of one key of the average size, which the planner's knots give */
static double
alt_keys(const SiftAggScope *sc, uint64 entries)
{
	return sc->knot_rows > 0 ? entries / sc->knot_rows : sc->nkeys;
}

/*
 * What the join as written costs for `keys` keys of the average size, in instructions: the planner's price at its knots (SiftAggScope), linear between them, flat past the last, which is
 * every key, and flat below the first: the entries of a read stand for a fraction of a key where its keys are smaller than the average one, and the join scans the table whatever the keys,
 * so the price of the first knot, which holds that scan, is what it costs (the line through the first two knots went on below the first and reached 0 for a tenant of 3 members in 200).
 */
static double
alt_price(const SiftAggScope *sc, double keys)
{
	int			i = 0;
	double		k = Max(Min(keys, sc->knot_k[sc->nknot - 1]), sc->knot_k[0]);

	if (sc->nknot == 1)
		return sc->knot_instr[0];
	while (i + 2 < sc->nknot && k > sc->knot_k[i + 1])
		i++;
	return sc->knot_instr[i] + (sc->knot_instr[i + 1] - sc->knot_instr[i]) * (k - sc->knot_k[i]) / (sc->knot_k[i + 1] - sc->knot_k[i]);
}

/* what the join as written costs for the keys the entries of a read stand for */
static double
join_price(const SiftAggScope *sc, const SiftScopeCounts *c)
{
	return alt_price(sc, alt_keys(sc, c->entries));
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
 * whatever they cost says so (sift.test_scope_entity).
 */
static void
run_join(AggExec *ex, const char *why, double price)
{
	const SiftAggScope *sc = ex->spec->scope;

	ex->scope_why = why;
	ex->scope_price = price;
	SIFT_INVARIANT(sc->alt->nkeys == ex->spec->nkeys && sc->alt->naggs == ex->spec->naggs && sc->alt->nparts == 0, "sift: the join a scope read holds has the groups of the node's own");
	ex->spec = sc->alt;
}

#define OLDER_FORMAT_WHY "scope segments of an older format (REINDEX)"
#define NO_RUN_WHY "no sidecar holds a run of the key"

/* the entries of the scope segments of an older format, which no read uses: the index is built again (REINDEX) to have the sidecars */
static bool
older_format(AggExec *ex)
{
	SiftDirSnapshot *dir;
	bool		found = false;

	dir = sift_dir_read(ex->index);
	for (int i = 0; i < dir->nsegs && !found; i++)
		found = sift_seg_is_scope(&dir->segs[i]);
	pfree(dir);
	return found;
}

/* Why the join runs where the sidecars have no run of the read's key (k is empty for several keys, whose runs are not counted) or the runs cost more than the join */
static const char *
join_why(AggExec *ex, const SideKeyCounts *k)
{
	const SiftAggScope *sc = ex->spec->scope;

	if (k->runs > 0)
		return join_cheaper_why(sc);
	if (older_format(ex))
		return OLDER_FORMAT_WHY;
	return sc->all || sc->nkeys != 1 ? join_cheaper_why(sc) : NO_RUN_WHY;
}

/* What the entity rows cannot answer: the rows hold no value of the key to group by, and several keys that count a row for each pair count it once */
static const char *
join_required(AggExec *ex)
{
	const SiftAggScope *sc = ex->spec->scope;

	if (sift_agg_scope_groups_by_key(ex->spec, sift_colsrc_layout(ex->index)))
		return "a group by the scope key";
	return !sc->once && (sc->all || sc->nkeys > 1) ? "a row for each pair of several keys" : NULL;
}

/* The read is of one key a sidecar can hold: an integer, or the code of a text, varchar or uuid key in its store (a key the store lacks has a negative code of the statement's own, and no run) */
static bool
key_in_store(const AggExec *ex)
{
	const SiftAggScope *sc = ex->spec->scope;

	return sc->nkeys == 1 && (ex->scope_km == NULL || sc->keys[0] >= 0);
}

/*
 * A scope read scans the entity rows of the members the scope lists, the runs of the key in the scope sidecars (col/side.h) answering from their cubes and the rest read by reference, or runs the
 * join as written when that costs less. The runs, their units and rows come from the sidecars, the units and rows of the segments that have none are read by reference (the key's share of them
 * as it is of the segments that have a sidecar), and the members are listed from the pairs. A read of several keys that counts a row for each pair is the join.
 */
static void
side_decide(AggExec *ex)
{
	const SiftAggScope *sc = ex->spec->scope;
	SideKeyCounts k = {0};
	SiftScopeCounts c = {.nkeys = sc->nkeys,.entry_instr = sc->entry_instr};
	double		share,
				rows,
				price;
	const char *why;

	why = join_required(ex);
	if (why != NULL)
	{
		if (sc->alt == NULL)
			ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("a scope read of %s needs the join", why),
							errhint("Rewrite the statement over the join to the scope relation, or SET sift.agg_scope to off.")));
		run_join(ex, why, 0.0);
		return;
	}
	if (sc->alt == NULL)
		return;
	if (key_in_store(ex))
		sift_side_key_counts(ex->index, sift_scope_ordinal(ex->index, sc->key_name), sc->keys[0], &k);
	share = k.side_rows > 0 ? (double) k.rows / k.side_rows : 1.0;
	rows = k.rows + share * k.other_rows;
	pair_counts(ex, &c);
	price = SIFT_SCOPE_SETUP_INSTR + SIFT_SCOPE_PREAGG_UNIT_INSTR * (double) k.runs + SIFT_SCOPE_UNIT_INSTR * (double) (k.runs > 0 ? k.other_units : k.other_units + k.units) +
		(SIFT_SCOPE_ENTRY_INSTR + sc->entry_instr + SIFT_SCOPE_MATCH_INSTR) * share * (double) k.other_rows + SIFT_SCOPE_PAIR_INSTR * (double) (c.pairs + c.scan_pairs);
	c.entries = (uint64) rows;
	if (!TEST_SCOPE_ENTITY && (k.runs == 0 || price > join_price(sc, &c)))
		run_join(ex, join_why(ex, &k), join_price(sc, &c));
	else
		ex->scope_price = price;
}

/* Decides how the scan reads the conjunct, before its source is opened: the entity rows of the members listed, or the join the node holds in the place of its spec */
void
scope_open(AggExec *ex, Snapshot snapshot)
{
	const SiftAggScope *sc = ex->spec->scope;

	if (ex->scope_share == NULL)
	{
		ex->scope_why = NULL;
		ex->scope_price = 0.0;
	}
	if (sc == NULL || ex->scope_share != NULL)
		return;
	if (sc->alt != NULL && TEST_SCOPE_JOIN)
		run_join(ex, "sift.test_scope_join", 0.0);
	else
		side_decide(ex);
	if (ex->scope_why == NULL && older_format(ex))
		ex->scope_why = OLDER_FORMAT_WHY;
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

/*
 * The conjunct of a text, varchar or uuid member: `member = ANY(strings)` over the dictionary of the leaf, as the keys of a text join are (join_filter_lut). The members of a read are the codes of
 * the map of the statement (scope_mm); the bytes of a value are those the unit's dictionary holds, whatever the column's type (a text with a varchar), because the pairs and the rows agree on equal values
 * being equal bytes (a deterministic collation: sift_keymap_column_refusal).
 */
static SiftAggPred *
member_lut(AggExec *ex, int *total, int leaf, const int64 *members, uint32 n, const char *desc)
{
	const SiftSrcLeaf *lf = &ex->lay->leaf[leaf];
	bool		uuid = lf->typid == UUIDOID;
	Datum	   *keys = palloc(Max(n, 1) * sizeof(Datum));
	ScalarArrayOpExpr *sa = makeNode(ScalarArrayOpExpr);
	Param	   *in = makeNode(Param);
	SiftAggPred *p;

	if (n == 0)
		return pred_node(ex, SAP_FALSE, total, -1, 0, 0, desc);
	for (uint32 i = 0; i < n; i++)
	{
		uint32		len;
		const char *str = sift_keymap_bytes(ex->scope_mm, members[i], &len);

		keys[i] = uuid ? PointerGetDatum(memcpy(palloc(UUID_LEN), str, UUID_LEN)) : PointerGetDatum(cstring_to_text_with_len(str, len));
	}
	sa->opno = uuid ? OpernameGetOprid(list_make1(makeString("=")), UUIDOID, UUIDOID) : TextEqualOperator;
	sa->opfuncid = uuid ? F_UUID_EQ : F_TEXTEQ;
	sa->hashfuncid = n >= 9 ? (uuid ? F_UUID_HASH : F_HASHTEXT) : InvalidOid;	/* the executor hashes a list of 9 or more */
	sa->negfuncid = InvalidOid;
	sa->useOr = true;
	sa->inputcollid = uuid ? InvalidOid : C_COLLATION_OID;
	sa->location = -1;
	in->paramkind = PARAM_EXEC;
	in->paramtype = uuid ? UUIDOID : TEXTOID;
	in->paramtypmod = -1;
	in->paramcollid = uuid ? InvalidOid : C_COLLATION_OID;
	in->location = -1;
	sa->args = list_make2(in, makeConst(uuid ? UUIDARRAYOID : TEXTARRAYOID, -1, InvalidOid, -1,
										PointerGetDatum(construct_array(keys, n, uuid ? UUIDOID : TEXTOID, uuid ? UUID_LEN : -1, false, uuid ? TYPALIGN_CHAR : TYPALIGN_INT)), false, false));
	p = pred_node(ex, SAP_LUT, total, leaf, 0, 0, desc);
	p->expr = (Node *) sa;
	p->argtype = lf->typid;
	register_pred(ex, p);
	pfree(keys);
	return p;
}

/* The conjunct that keeps the rows of the scope: added to the spec after its own, as the key conjuncts of the joins are (join_filters) */
void
scope_filter(AggExec *ex, int *total, List **add)
{
	const SiftAggScope *sc = ex->spec->scope;
	const int64 *members;
	SiftAggPred *p;
	const char *desc;
	uint32		n;

	ex->scope_preds = 0;
	ex->scope_pred = NULL;
	if (ex->scope_share == NULL)
	{
		ex->scope_listed = listed_members(ex, ex->snap, &n);
		ex->scope_members = n;
	}
	members = ex->scope_listed;
	n = ex->scope_members;
	ex->scope_preds = 1;
	desc = n == 0 ? "no member has a pair with the scope keys" : psprintf("member of scope %s keys (%s)", sc->key_name, keys_text(sc->keys, sc->nkeys));
	p = ex->scope_mm != NULL ? member_lut(ex, total, sc->fk, members, n, desc) : pred_in(ex, total, sc->fk, false, members, n, desc);
	ex->scope_pred = p;
	*add = lappend(*add, p);
}

/*
 * The scan of the entity rows of the members the scope lists, for one key: the source is told which key it is, so that its runs (col/side.h) stand for their units where the journal says no pair of
 * their members changed; the member conjunct is still the filter of every other unit, and the runs answer for it (SiftSrcPrune.scope).
 */
void
scope_side_open(AggExec *ex)
{
	const SiftAggScope *sc = ex->spec->scope;

	ex->scope_side = false;
	if (sc == NULL || ex->scope_pred == NULL || sc->all || !key_in_store(ex) || TEST_SCOPE_NO_SIDE)
		return;
	ex->scope_side = sift_colsrc_scope_side(ex->src, sc->key_name, sc->keys[0], ex->scope_km, ex->scope_mm, ex->scope_listed, ex->scope_members);
}

/* The spec without the member conjunct of a text, varchar or uuid member, which is no zone comparison and so no shape the pre-aggregates are planned for: the runs answer for it, and the other units are scanned (preagg_unit_try) */
const SiftAggSpec *
scope_spec_plain(AggExec *ex)
{
	SiftAggSpec *c;
	int			k = 0;

	if (ex->scope_mm == NULL || ex->scope_pred == NULL)
		return ex->spec;
	c = palloc(sizeof(SiftAggSpec));
	*c = *ex->spec;
	c->pred = palloc(Max(ex->spec->npred, 1) * sizeof(SiftAggPred *));
	for (int i = 0; i < ex->spec->npred; i++)
		if (ex->spec->pred[i] != ex->scope_pred)
			c->pred[k++] = ex->spec->pred[i];
	c->npred = k;
	return c;
}

/*
 * What the workers of a scan take from the leader for the scope conjunct: the members listed from the snapshot of the statement and the pairs of the relations at that moment (a worker that made
 * them again could find another: a pair edited in between), or the join as written, which the leader runs in the place of the node's spec.
 */
typedef struct ScopeShare
{
	int64		join;			/* the join as written, the alternative of the node's spec, which the leader runs in its place: nothing follows */
	int64		nkeys;
	int64		nlisted;
	int64		nmm;			/* a text, varchar or uuid member: the values of its map, in the order of their codes, after the listed members: a length and the bytes of each */
	int64		mm_typid;		/* InvalidOid: the member is an integer */
	int64		mm_collation;
} ScopeShare;

StringInfo
scope_share_build(AggExec *ex)
{
	const SiftAggScope *sc = ex->unpacked != NULL ? ex->unpacked->scope : NULL;
	StringInfo	b = makeStringInfo();
	ScopeShare	h = {0};

	if (sc == NULL)
		return NULL;
	h.join = ex->spec->scope == NULL;
	if (!h.join)
	{
		h.nkeys = sc->nkeys;
		h.nlisted = ex->scope_members;
		h.mm_typid = ex->scope_mm != NULL ? sift_keymap_typid(ex->scope_mm) : InvalidOid;
		h.mm_collation = ex->scope_mm != NULL ? sift_keymap_collation(ex->scope_mm) : InvalidOid;
		h.nmm = ex->scope_mm != NULL ? sift_keymap_transient_n(ex->scope_mm) : 0;
	}
	appendBinaryStringInfo(b, (const char *) &h, sizeof(h));
	if (!h.join)
	{
		appendBinaryStringInfo(b, (const char *) sc->keys, sc->nkeys * sizeof(int64));
		appendBinaryStringInfo(b, (const char *) ex->scope_listed, ex->scope_members * sizeof(int64));
		for (int64 i = 0; i < h.nmm; i++)
		{
			uint32		len;
			const char *str = sift_keymap_bytes(ex->scope_mm, -1 - i, &len);

			appendBinaryStringInfo(b, (const char *) &len, sizeof(len));
			appendBinaryStringInfo(b, str, len);
		}
	}
	return b;
}

/* does the leader run the join as written in the place of the node's spec */
bool
scope_share_is_join(const void *share)
{
	return ((const ScopeShare *) share)->join != 0;
}

static int64 *
take_i64s(const char **p, int64 n)
{
	int64	   *v = palloc(Max(n, 1) * sizeof(int64));

	memcpy(v, *p, n * sizeof(int64));
	*p += n * sizeof(int64);
	return v;
}

/* A worker takes the leader's decision (scope_keys_init: with the keys, which a statement with parameters evaluates in the leader only) */
void
scope_share_adopt(AggExec *ex)
{
	const char *p = ex->scope_share;
	ScopeShare	h;
	SiftAggScope *sc = ex->spec->scope;
	MemoryContext old;

	memcpy(&h, p, sizeof(h));
	p += sizeof(h);
	old = MemoryContextSwitchTo(GetMemoryChunkContext(sc));
	sc->keys = take_i64s(&p, h.nkeys);
	sc->nkeys = h.nkeys;
	MemoryContextSwitchTo(old);
	ex->scope_members = h.nlisted;
	ex->scope_listed = take_i64s(&p, h.nlisted);
	if (OidIsValid((Oid) h.mm_typid))
		ex->scope_mm = sift_keymap_transient_of(h.mm_typid, h.mm_collation);
	for (int64 i = 0; i < h.nmm; i++)
	{
		uint32		len;
		int64		code;

		memcpy(&len, p, sizeof(len));
		(void) sift_keymap_code_bytes(ex->scope_mm, p + sizeof(len), len, &code);
		SIFT_INVARIANT(code == -1 - i, "sift: a worker's map of the scope members has other codes than its leader's");
		p += sizeof(len) + len;
	}
}
