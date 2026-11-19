/*
 * col/scan/scope.c - what the reader of a scope index asks of its scope relation (col/scope.h)
 *
 * The pairs of the scope relation under a snapshot: the members it lists for the keys, the groups of a key, and every pair (the scope sidecars are made from them). A scan that has a scope
 * conjunct reads the entity rows of those members (INTERFACE section 6, docs/COL-CONNECT.md "The rule").
 */
/* check-memory: allow-allocs the arrays follow the keys of one scope conjunct, the pairs of the keys it names and the members the caps of the statement allow, not the rows of the table */
#include "postgres.h"

#include "access/genam.h"
#include "access/table.h"
#include "access/tableam.h"
#include "access/transam.h"
#include "common/int.h"
#include "core/cmp.h"
#include "core/compat.h"
#include "executor/tuptable.h"
#include "optimizer/optimizer.h"
#include "miscadmin.h"
#include "storage/sinval.h"
#include "utils/memutils.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"

#include "col/am.h"
#include "col/seg.h"
#include "col/scope.h"
#include "col/wbuf.h"

typedef struct ScopePairs
{
	SiftScopePair  *p;
	uint32		n;
	uint32		cap;
	uint32		max;			/* the most pairs held (0: no limit); past it pairs are counted in n but not kept, and over is set */
	bool		over;
} ScopePairs;

static int
scope_ordinal(const SiftScopeSpecs *sp, const char *key_name)
{
	for (int i = 0; sp != NULL && i < sp->n; i++)
		if (strcmp(sp->s[i]->key_name, key_name) == 0)
			return i;
	return -1;
}

/* The ordinal of the scope key_name of the index, or the error of an index that has none by that name */
int
sift_scope_ordinal(Relation index, const char *key_name)
{
	int			ord = scope_ordinal(sift_scope_specs(index), key_name);

	if (ord < 0)
		ereport(ERROR, (errcode(ERRCODE_UNDEFINED_OBJECT), errmsg("index \"%s\" has no scope \"%s\"", RelationGetRelationName(index), key_name)));
	return ord;
}

static int
cmp_i64(const void *a, const void *b)
{
	return pg_cmp_s64(*(const int64 *) a, *(const int64 *) b);
}

#define PAIR_BY_MEMBER(x, y) ((x)->member != (y)->member ? (x)->member < (y)->member : (x)->key < (y)->key)
#define PAIR_BY_KEY(x, y) ((x)->key != (y)->key ? (x)->key < (y)->key : (x)->member < (y)->member)
SIFT_DEFINE_SORT(pairs_by_member_cmp, SiftScopePair, PAIR_BY_MEMBER)
SIFT_DEFINE_SORT(pairs_by_key, SiftScopePair, PAIR_BY_KEY)

static void *
pairs_copy(size_t bytes)
{
	return bytes <= MaxAllocSize ? palloc_extended(bytes, MCXT_ALLOC_NO_OOM) : NULL;
}

#define PAIR_MEMBER(x) ((x)->member)
#define PAIR_KEY(x) ((x)->key)
#define PAIR_NOTIE(x) 0
/* a pair list by (member, key): the radix sort takes a few passes over thousands of pairs, where the comparison sort takes a call for each of some twenty steps of each */
SIFT_DEFINE_RADIX_SORT(pairs_by_member, SiftScopePair, PAIR_MEMBER, PAIR_KEY, PAIR_NOTIE, pairs_copy, pfree, pairs_by_member_cmp)

/* the distinct values of a, ascending: sorts in place and returns how many */
static uint32
sort_unique(int64 *a, uint32 n)
{
	uint32		k = 0;

	if (n > 0)
		sift_sort_i64(a, n);
	for (uint32 i = 0; i < n; i++)
		if (k == 0 || a[i] != a[k - 1])
			a[k++] = a[i];
	return k;
}

/* the distinct keys ascending, a copy; NULL for every key (keys NULL) */
static int64 *
sorted_keys(const int64 *keys, int nkeys, uint32 *n)
{
	int64	   *sorted;

	*n = 0;
	if (keys == NULL)
		return NULL;
	sorted = memcpy(palloc(Max(nkeys, 1) * sizeof(int64)), keys, nkeys * sizeof(int64));
	*n = sort_unique(sorted, nkeys);
	return sorted;
}

static bool
has_i64(const int64 *a, uint32 n, int64 v)
{
	return n > 0 && bsearch(&v, a, n, sizeof(int64), cmp_i64) != NULL;
}

static void
pairs_add(ScopePairs *ps, int64 member, int64 key)
{
	if (ps->max > 0 && ps->n >= ps->max)
	{
		ps->over = true;
		return;
	}
	if (ps->n == ps->cap)
	{
		ps->cap = ps->cap ? ps->cap * 2 : 64;
		ps->p = ps->p ? repalloc(ps->p, ps->cap * sizeof(SiftScopePair)) : palloc(ps->cap * sizeof(SiftScopePair));
	}
	ps->p[ps->n++] = (SiftScopePair) {member, key};
}

static uint64
pairs_unique(SiftScopePair *p, uint64 n)
{
	uint64		k = 0;

	if (n > 0)
		pairs_by_member(p, n);
	for (uint64 i = 0; i < n; i++)
		if (k == 0 || PAIR_BY_MEMBER(&p[k - 1], &p[i]))
			p[k++] = p[i];
	return k;
}

/* The pairs of the list ascending by member, each once: a relation lists a pair once for each of its rows, the leaves of the journal index and the probes of the btree too */
static void
pairs_distinct(ScopePairs *ps)
{
	ps->n = (uint32) pairs_unique(ps->p, ps->n);
}

/* ---- the pairs of the scope relation under the snapshot ---- */

typedef struct PairScan
{
	Relation	rel;
	Oid			btree;
	AttrNumber	member;
	AttrNumber	skey;
	SiftKeyMap *kma;			/* the member column is text, varchar or uuid: its values are the codes of this map of the statement (NULL: an integer column) */
	SiftKeyMap *km;				/* the scope key column is text, varchar or uuid: its values are the codes of this map (NULL: an integer column) */
	Snapshot	snap;
	const int64 *keys;			/* the scope keys asked for, sorted and distinct; NULL: every key */
	uint32		nkeys;
	ScopePairs	out;
	int64		cur;			/* the key a probe asks for */
	SiftConnPred pred;			/* a probe of the journal index: the predicate of the btree, which the rows of the journal index need not satisfy */
} PairScan;

static bool
wanted(const PairScan *ps, int64 key)
{
	return ps->keys == NULL || has_i64(ps->keys, ps->nkeys, key);
}

static bool
btree_leads_with(Oid btree, AttrNumber att)
{
	Relation	idx = index_open(btree, AccessShareLock);
	bool		leads = idx->rd_index->indkey.values[0] == att;

	index_close(idx, NoLock);
	return leads;
}

static void
scan_pair(void *arg, int64 member, int64 key)
{
	PairScan   *ps = arg;

	if (wanted(ps, key))
		pairs_add(&ps->out, member, key);
}

static bool
probe_of_key(void *arg, TupleTableSlot *slot)
{
	PairScan   *ps = arg;
	int64		m;

	if (sift_conn_slot_code(slot, ps->member, ps->kma, &m))
		pairs_add(&ps->out, m, ps->cur);
	return true;
}

static bool
probe_of_member(void *arg, TupleTableSlot *slot)
{
	PairScan   *ps = arg;
	int64		m,
				k;

	if (sift_conn_pred_test(&ps->pred, slot) && sift_conn_slot_code(slot, ps->member, ps->kma, &m) && sift_conn_slot_code(slot, ps->skey, ps->km, &k) && wanted(ps, k))
		pairs_add(&ps->out, m, k);
	return true;
}

/* The btree asked for each value of the column it leads with (probe of key or of member), each hit handed to each */
static void
probe_all(PairScan *ps, AttrNumber lead, const int64 *vals, uint32 n, SiftConnProbeFn each)
{
	SiftConnProbe *p = sift_conn_probe_begin_on(CurrentMemoryContext, ps->rel, ps->btree, lead, ps->snap);
	SiftConnProbeScan s;

	if (lead == ps->skey && ps->km != NULL)
		sift_conn_probe_codec(p, ps->km);
	sift_conn_probe_open(p, &s);
	if (ps->km != NULL)
		sift_keymap_hold(ps->km);
	for (uint32 i = 0; i < n; i++)
	{
		ps->cur = vals[i];
		sift_conn_probe_key(p, &s, ps->cur, each, ps);
	}
	if (ps->km != NULL)
		sift_keymap_release(ps->km);
	sift_conn_probe_close(&s);
}

/* A pair read from the leaves of the journal index of the relation costs about this (138 instructions: 16 million for 116,000 pairs; the heap read of FILL_SCAN_NS_PER_PARENT_ROW in conn_fill.c is three times that), a probe of the btree about 3,500 */
#define LEAF_PAIR_NS	45
#define SCAN_NS		130
#define PROBE_NS		3500

static bool
leaf_pairs(PairScan *ps)
{
	Oid			journal = sift_scope_pairs_journal(ps->rel, ps->member, ps->skey, ps->btree);
	SiftScopePair *p;
	uint32		n;

	if (!OidIsValid(journal) || !lookcol_pairs(ps->rel, journal, ps->snap, ps->member, ps->skey, ps->kma, ps->km, ps->keys, ps->nkeys, &p, &n))
		return false;
	for (uint32 i = 0; i < n; i++)
		pairs_add(&ps->out, p[i].member, p[i].key);
	return true;
}

/*
 * The pairs with one of the keys (any key: keys NULL), ascending by member: the btree asked for each key when it leads with the scope key and the keys are few, else the leaves of the relation's journal index (a pair with a NULL in a column is no pair)
 * (a sift_col index of both columns), else the relation read once
 */
static void
pairs_of_keys(PairScan *ps)
{
	if (ps->keys != NULL && btree_leads_with(ps->btree, ps->skey) && (double) ps->nkeys * PROBE_NS < sift_conn_rows_of(ps->rel) * LEAF_PAIR_NS)
		probe_all(ps, ps->skey, ps->keys, ps->nkeys, probe_of_key);
	else if (!leaf_pairs(ps))
		sift_conn_scan_pairs(ps->rel, ps->btree, ps->member, ps->kma, ps->skey, ps->km, ps->snap, scan_pair, ps);
	pairs_distinct(&ps->out);
}

/* The members asked of the relation's journal index in one search: its rows (a journal index is led by the member and holds every row of a pair, soft deleted ones too) are tested against the predicate of the btree */
static void
probe_journal(PairScan *ps, Oid journal, const int64 *members, uint32 n)
{
	SiftConnProbe *p = sift_conn_probe_begin_on(CurrentMemoryContext, ps->rel, journal, ps->member, ps->snap);
	SiftConnProbeScan s;

	sift_conn_pred_begin(&ps->pred, ps->btree);
	sift_conn_probe_open(p, &s);
	sift_conn_probe_keys(p, &s, members, n, probe_of_member, ps);
	sift_conn_probe_close(&s);
	sift_conn_pred_end(&ps->pred);
}

/* What the pairs of the keys cost to ask for, in ns: the btree for each key when it leads with the scope key, else the leaves of the relation's journal index, else the relation read (pairs_of_keys) */
static double
keys_ns(const PairScan *ps, double rows)
{
	double		read = rows * (OidIsValid(sift_scope_pairs_journal(ps->rel, ps->member, ps->skey, ps->btree)) ? LEAF_PAIR_NS : SCAN_NS);

	return ps->keys != NULL && btree_leads_with(ps->btree, ps->skey) ? Min(read, (double) ps->nkeys * PROBE_NS) : read;
}

/*
 * The pairs of the members, by asking an index for them: the btree for each member when it leads with the member, else the journal index of the relation in one search (it is led by the member
 * whichever way the btree is), whichever is cheaper than the pairs of the keys (keys_ns): thousands of changed members cost more to ask for than the leaves of the keys' pairs to read. False: neither is
 */
static bool
probe_members(PairScan *ps, const int64 *members, uint32 n)
{
	double		read = keys_ns(ps, sift_conn_rows_of(ps->rel));
	Oid			journal;

	if (btree_leads_with(ps->btree, ps->member) && sift_conn_probe_cheaper_than(n, read))
		probe_all(ps, ps->member, members, n, probe_of_member);
	else if (OidIsValid(journal = sift_scope_journal_index(ps->rel, ps->member, ps->skey, ps->btree)) && sift_conn_journal_cheaper_than(n, read))
		probe_journal(ps, journal, members, n);
	else
		return false;
	pairs_distinct(&ps->out);
	return true;
}

/*
 * Is the member of a pair (its member key: the integer, or the code of the map of a text, varchar or uuid member) one of the members, ascending: the integers of the members, or for a text, varchar or
 * uuid member their journal words (sift_member_hash), which two members may share: the set is of the members the journal names and every other member with one of their words
 */
static bool
in_set(SiftKeyMap *kma, const int64 *members, uint32 n, int64 member)
{
	return has_i64(members, n, kma != NULL ? (int64) sift_keymap_member_hash(kma, member) : member);
}

/* The pairs of the members with one of the keys: probed (probe_members), else the pairs of the keys filtered. The members of a text, varchar or uuid column are known by their words, which no btree asks for */
static void
pairs_of_members(PairScan *ps, const int64 *members, uint32 n)
{
	uint32		k = 0;

	if (ps->kma == NULL && probe_members(ps, members, n))
		return;
	pairs_of_keys(ps);
	for (uint32 i = 0; i < ps->out.n; i++)
		if (in_set(ps->kma, members, n, ps->out.p[i].member))
			ps->out.p[k++] = ps->out.p[i];
	ps->out.n = k;
}

/* The scope relation and its btree of a declared scope; the relation is open (AccessShare) */
static Oid
scope_relation(Relation heap, Relation index, int ord, SiftKeyMap *km, SiftKeyMap *mm, PairScan *ps, const int64 *keys, uint32 nkeys, Snapshot snap)
{
	SiftScopeSpecs *sp = sift_scope_specs(index);
	Oid			btree = sift_scope_relation_index(heap, index, sp->s[ord], &ps->rel, &ps->member, &ps->skey);

	ps->btree = btree;
	ps->km = km;
	/* the members of a read are one map: the pairs and the rows of the buffers and the segments must give a value one code, so the first call makes it and the next ones are given it */
	ps->kma = mm != NULL ? mm : sift_keymap_transient(ps->rel, ps->member);
	ps->snap = snap;
	ps->keys = keys;
	ps->nkeys = nkeys;
	return btree;
}

/* The first pair of the list (ascending by member) that has member or a larger one */
static uint32
first_of_member(const ScopePairs *ps, int64 member)
{
	uint32		lo = 0,
				hi = ps->n;

	while (lo < hi)
	{
		uint32		mid = lo + (hi - lo) / 2;

		if (ps->p[mid].member < member)
			lo = mid + 1;
		else
			hi = mid;
	}
	return lo;
}

/* A relation of the second path ready to read: hop 1 the joined relation, its pairs (via, key); hop 2 the via relation, its pairs (member, via) */
static void
hop_scan(Relation heap, Relation index, const SiftScopeSpec *s, int hop, PairScan *p, Snapshot snap, const int64 *keys, uint32 nkeys, SiftKeyMap *km, SiftKeyMap *mm)
{
	p->snap = snap;
	p->keys = keys;
	p->nkeys = nkeys;
	/* the scope key of the joined path is that of the pairs relation, and the member of the via relation is its member: they are coded by the maps of the read; the group column is an integer */
	if (hop == 1)
	{
		p->btree = sift_scope_hop_index(heap, index, s, 1, &p->rel, &p->member, &p->skey);
		p->km = km;
	}
	else
	{
		p->btree = sift_scope_hop_index(heap, index, s, 2, &p->rel, &p->skey, &p->member);
		p->kma = mm;
	}
}

/* The distinct values of the member column of the pairs, or of their key column */
static int64 *
pair_values(const ScopePairs *ps, bool key, uint32 *n)
{
	int64	   *v = palloc(Max(ps->n, 1) * sizeof(int64));

	for (uint32 i = 0; i < ps->n; i++)
		v[i] = key ? ps->p[i].key : ps->p[i].member;
	*n = sift_conn_keys_sort_unique(v, ps->n);
	return v;
}

/*
 * The pairs a scope of two paths adds to those of its pairs relation, the keys of the joined relation's rows (via, key) for each member of the via relation with that via value: of the
 * members asked for (members NULL: of every member) that have a key in keys. The joined relation is asked for the via values of the via relation, or the via relation for those of the
 * joined relation, whichever side is the smaller. May hold a pair twice: the caller keeps the distinct ones it needs.
 */
static void
add_joined_pairs(Relation heap, Relation index, const SiftScopeSpec *s, PairScan *ps, const int64 *members, uint32 nmembers)
{
	PairScan	g = {0},
				v = {0};
	int64	   *vias;
	uint32		nvia;

	if (members == NULL)
	{
		hop_scan(heap, index, s, 1, &g, ps->snap, ps->keys, ps->nkeys, ps->km, ps->kma);
		pairs_of_keys(&g);
		vias = pair_values(&g.out, false, &nvia);
		hop_scan(heap, index, s, 2, &v, ps->snap, vias, nvia, ps->km, ps->kma);
		pairs_of_keys(&v);
	}
	else
	{
		hop_scan(heap, index, s, 2, &v, ps->snap, NULL, 0, ps->km, ps->kma);
		pairs_of_members(&v, members, nmembers);
		vias = pair_values(&v.out, true, &nvia);
		hop_scan(heap, index, s, 1, &g, ps->snap, ps->keys, ps->nkeys, ps->km, ps->kma);
		pairs_of_members(&g, vias, nvia);
	}
	table_close(g.rel, NoLock);
	table_close(v.rel, NoLock);
	for (uint32 i = 0; i < v.out.n; i++)
		for (uint32 j = first_of_member(&g.out, v.out.p[i].key); j < g.out.n && g.out.p[j].member == v.out.p[i].key; j++)
			pairs_add(&ps->out, v.out.p[i].member, g.out.p[j].key);
}

/*
 * The distinct pairs of the scope under snap with one of the keys, ascending by member: those of the members only (members NULL: every member's). At most max of them are held (0: any; over is set
 * when there are more). Of two paths the pairs are those of the pairs relation and of the joined path.
 */
static ScopePairs
scope_pairs(Relation heap, Relation index, int ord, SiftKeyMap *km, SiftKeyMap *mm, Snapshot snap, const int64 *keys, uint32 nkeys, const int64 *members, uint32 nmembers, uint32 max)
{
	const SiftScopeSpec *s = sift_scope_specs(index)->s[ord];
	PairScan	ps = {0};

	scope_relation(heap, index, ord, km, mm, &ps, keys, nkeys, snap);
	ps.out.max = max;
	if (members == NULL)
		pairs_of_keys(&ps);
	else
		pairs_of_members(&ps, members, nmembers);
	table_close(ps.rel, NoLock);
	if (s->joined != NULL)
	{
		add_joined_pairs(heap, index, s, &ps, members, nmembers);
		pairs_distinct(&ps.out);
	}
	return ps.out;
}

static int
pair_by_key(const void *a, const void *b)
{
	const SiftScopePair *x = a,
			   *y = b;

	if (x->key != y->key)
		return x->key < y->key ? -1 : 1;
	return (x->member > y->member) - (x->member < y->member);
}

SiftScopePair *
sift_scope_all_pairs(Relation heap, Relation index, int ord, Snapshot snap, uint64 *n, SiftKeyMap **members)
{
	PairScan	ps = {0};
	SiftKeyMap *scanned,
			   *writer = NULL;
	ScopePairs	pairs;

	scope_relation(heap, index, ord, NULL, NULL, &ps, NULL, 0, NULL);
	scanned = sift_keymap_transient(ps.rel, ps.skey);
	*members = sift_keymap_transient(ps.rel, ps.member);
	if (scanned != NULL)
		writer = sift_keymap_open(ps.rel, ps.skey, true);
	table_close(ps.rel, NoLock);
	pairs = scope_pairs(heap, index, ord, scanned, *members, snap, NULL, 0, NULL, 0, 0);
	if (scanned != NULL)
	{
		const uint32 *codes = sift_keymap_store_codes(writer, scanned);

		for (uint32 i = 0; i < pairs.n; i++)
			pairs.p[i].key = codes[-1 - pairs.p[i].key];
	}
	qsort(pairs.p, pairs.n, sizeof(SiftScopePair), pair_by_key);
	*n = pairs.n;
	return pairs.p;
}

/* The groups (the via values) the keys have under snap: the member column of the joined relation's pairs with one of the keys, of a scope of two paths */
int64 *
sift_scope_groups(Relation heap, Relation index, int ord, SiftKeyMap *km, Snapshot snap, const int64 *keys, uint32 nkeys, uint32 *n)
{
	PairScan	g = {0};
	int64	   *groups;

	hop_scan(heap, index, sift_scope_specs(index)->s[ord], 1, &g, snap, keys, nkeys, km, NULL);
	pairs_of_keys(&g);
	groups = pair_values(&g.out, false, n);
	table_close(g.rel, NoLock);
	return groups;
}

SiftKeyMap *
sift_scope_keymap(Relation heap, Relation index, const char *key_name)
{
	PairScan	ps = {0};
	SiftKeyMap *km;

	(void) scope_relation(heap, index, scope_ordinal(sift_scope_specs(index), key_name), NULL, NULL, &ps, NULL, 0, NULL);
	km = sift_keymap_open(ps.rel, ps.skey, false);
	table_close(ps.rel, NoLock);
	return km;
}

SiftKeyMap *
sift_scope_member_map(Relation heap, Relation index, const char *key_name)
{
	PairScan	ps = {0};
	SiftKeyMap *mm;

	(void) scope_relation(heap, index, scope_ordinal(sift_scope_specs(index), key_name), NULL, NULL, &ps, NULL, 0, NULL);
	mm = sift_keymap_transient(ps.rel, ps.member);
	table_close(ps.rel, NoLock);
	return mm;
}

int64 *
sift_scope_members(Relation heap, Relation index, const char *key_name, SiftKeyMap *km, SiftKeyMap *mm, Snapshot snap, const int64 *keys, int nkeys, uint32 *n)
{
	uint32		nk;
	int64	   *sorted = sorted_keys(keys, nkeys, &nk);
	ScopePairs	pairs = scope_pairs(heap, index, sift_scope_ordinal(index, key_name), km, mm, snap, sorted, nk, NULL, 0, 0);

	return pair_values(&pairs, false, n);
}

Oid
sift_scope_pairs_index(Relation heap, Relation index, const char *key_name, Relation *rel, AttrNumber *member, AttrNumber *skey)
{
	PairScan	ps = {0};
	int			ord = sift_scope_ordinal(index, key_name);

	scope_relation(heap, index, ord, NULL, NULL, &ps, NULL, 0, NULL);
	*rel = ps.rel;
	*member = ps.member;
	*skey = ps.skey;
	/* the leaves of one journal list the pairs of one relation; the members of a scope of two paths are listed by sift_scope_members, and the leaves hold the strings of a text key, not its codes */
	return sift_scope_specs(index)->s[ord]->joined != NULL || ps.kma != NULL || sift_keymap_type(get_atttype(RelationGetRelid(ps.rel), ps.skey)) ? InvalidOid :
		sift_scope_pairs_journal(ps.rel, ps.member, ps.skey, ps.btree);
}
