/*
 * col/scan/scope.c - what the reader of a scope index asks of the index and of its scope relation (col/scope.h)
 *
 * A scan that has a scope conjunct reads the scope units of its keys, less the members that changed in the scope relation since the stamp of the units, plus the rows no scope entry describes
 * (the delta): those of the write buffers and, read by member key from the entity segments, those of the changed members that have a pair with a key now (the patch), each expanded by the pairs
 * of its member under the statement snapshot: INTERFACE section 6, docs/COL-CONNECT.md "The rule". A fact of a member that has a pair with several of the keys counts once for IN and EXISTS
 * (section 6b): the units of all but the least of its keys leave it out, and the delta has the least key only.
 * Whenever the stored read is not exact the scan reads the entity rows of the members the scope relation lists for the keys, and says why.
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

/* a key whose units leave members out is one conjunct of the scan: the keys of an IN list that have them are at most this many */
#define SCOPE_DROP_KEYS 64

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

/* The first leaf among leaves[0..n) the scope segments keep no values of, as the name of its column or path; NULL when they keep all (leaves beyond the index's are the scan's own) */
static const char *
unit_missing(Relation index, int scope, const int *leaves, int n)
{
	const SiftColRel *r = sift_col_rel(index);	/* check-memory: allow nothing below reads the catalog */

	for (int i = 0; i < n; i++)
	{
		const SiftColDef *d;

		if (leaves[i] >= r->schema->ncols || r->scope_cols[scope][leaves[i]])
			continue;
		d = &r->schema->cols[leaves[i]];
		return d->path[0] ? d->path : NameStr(TupleDescAttr(RelationGetDescr(index), d->attno - 1)->attname);
	}
	return NULL;
}

void
sift_scope_columns(Relation index, const char *key_name, int *kept, int *all)
{
	int			scope = scope_ordinal(sift_scope_specs(index), key_name);
	const SiftColRel *r = sift_col_rel(index);	/* check-memory: allow nothing below reads the catalog */
	int			natts = RelationGetNumberOfAttributes(index);

	*all = natts;
	*kept = 0;
	for (int a = 1; a <= natts; a++)
		for (int i = 0; i < r->schema->ncols; i++)
			if (r->schema->cols[i].attno == a && !(r->schema->cols[i].flags & SIFT_COLD_DERIVED) && r->scope_cols[scope][i])
			{
				(*kept)++;
				break;
			}
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

static int64 *
union_i64(const int64 *a, uint32 na, const int64 *b, uint32 nb, uint32 *n)
{
	int64	   *u = palloc(Max(na + nb, 1) * sizeof(int64));

	if (na > 0)
		memcpy(u, a, na * sizeof(int64));
	if (nb > 0)
		memcpy(u + na, b, nb * sizeof(int64));
	*n = sift_conn_keys_sort_unique(u, na + nb);
	return u;
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

uint64
sift_scope_pairs_distinct(SiftScopePair *p, uint64 n)
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
	ps->n = (uint32) sift_scope_pairs_distinct(ps->p, ps->n);
}

/* ---- the pairs of the scope relation under the snapshot ---- */

typedef struct PairScan
{
	Relation	rel;
	Oid			btree;
	AttrNumber	member;
	AttrNumber	skey;
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

	if (sift_conn_slot_key(slot, ps->member, &m))
		pairs_add(&ps->out, m, ps->cur);
	return true;
}

static bool
probe_of_member(void *arg, TupleTableSlot *slot)
{
	PairScan   *ps = arg;
	int64		m,
				k;

	if (sift_conn_pred_test(&ps->pred, slot) && sift_conn_slot_key(slot, ps->member, &m) && sift_conn_slot_code(slot, ps->skey, ps->km, &k) && wanted(ps, k))
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
	for (uint32 i = 0; i < n; i++)
	{
		ps->cur = vals[i];
		sift_conn_probe_key(p, &s, ps->cur, each, ps);
	}
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

	/* the leaves of the journal index hold the strings of the key, not its codes */
	if (ps->km != NULL || !OidIsValid(journal) || !lookcol_pairs(ps->rel, journal, ps->snap, ps->member, ps->skey, ps->keys, ps->nkeys, &p, &n))
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
		sift_conn_scan_pairs(ps->rel, ps->btree, ps->member, ps->skey, ps->km, ps->snap, scan_pair, ps);
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
	double		read = rows * (ps->km == NULL && OidIsValid(sift_scope_pairs_journal(ps->rel, ps->member, ps->skey, ps->btree)) ? LEAF_PAIR_NS : SCAN_NS);

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

/* The pairs of the members with one of the keys: probed (probe_members), else the pairs of the keys filtered */
static void
pairs_of_members(PairScan *ps, const int64 *members, uint32 n)
{
	uint32		k = 0;

	if (probe_members(ps, members, n))
		return;
	pairs_of_keys(ps);
	for (uint32 i = 0; i < ps->out.n; i++)
		if (has_i64(members, n, ps->out.p[i].member))
			ps->out.p[k++] = ps->out.p[i];
	ps->out.n = k;
}

/* The scope relation and its btree of a declared scope; the relation is open (AccessShare) */
static Oid
scope_relation(Relation heap, Relation index, int ord, SiftKeyMap *km, PairScan *ps, const int64 *keys, uint32 nkeys, Snapshot snap)
{
	SiftScopeSpecs *sp = sift_scope_specs(index);
	Oid			btree = sift_scope_relation_index(heap, index, sp->s[ord], &ps->rel, &ps->member, &ps->skey);

	ps->btree = btree;
	ps->km = km;
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
hop_scan(Relation heap, Relation index, const SiftScopeSpec *s, int hop, PairScan *p, Snapshot snap, const int64 *keys, uint32 nkeys)
{
	p->snap = snap;
	p->keys = keys;
	p->nkeys = nkeys;
	if (hop == 1)
		p->btree = sift_scope_hop_index(heap, index, s, 1, &p->rel, &p->member, &p->skey);
	else
		p->btree = sift_scope_hop_index(heap, index, s, 2, &p->rel, &p->skey, &p->member);
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
		hop_scan(heap, index, s, 1, &g, ps->snap, ps->keys, ps->nkeys);
		pairs_of_keys(&g);
		vias = pair_values(&g.out, false, &nvia);
		hop_scan(heap, index, s, 2, &v, ps->snap, vias, nvia);
		pairs_of_keys(&v);
	}
	else
	{
		hop_scan(heap, index, s, 2, &v, ps->snap, NULL, 0);
		pairs_of_members(&v, members, nmembers);
		vias = pair_values(&v.out, true, &nvia);
		hop_scan(heap, index, s, 1, &g, ps->snap, ps->keys, ps->nkeys);
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
scope_pairs(Relation heap, Relation index, int ord, SiftKeyMap *km, Snapshot snap, const int64 *keys, uint32 nkeys, const int64 *members, uint32 nmembers, uint32 max)
{
	const SiftScopeSpec *s = sift_scope_specs(index)->s[ord];
	PairScan	ps = {0};

	scope_relation(heap, index, ord, km, &ps, keys, nkeys, snap);
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

/* The groups (the via values) the keys have under snap: the member column of the joined relation's pairs with one of the keys, of a scope of two paths */
int64 *
sift_scope_groups(Relation heap, Relation index, int ord, Snapshot snap, const int64 *keys, uint32 nkeys, uint32 *n)
{
	PairScan	g = {0};
	int64	   *groups;

	hop_scan(heap, index, sift_scope_specs(index)->s[ord], 1, &g, snap, keys, nkeys);
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

	(void) scope_relation(heap, index, scope_ordinal(sift_scope_specs(index), key_name), NULL, &ps, NULL, 0, NULL);
	km = sift_keymap_open(ps.rel, ps.skey, false);
	table_close(ps.rel, NoLock);
	return km;
}

int64 *
sift_scope_members(Relation heap, Relation index, const char *key_name, SiftKeyMap *km, Snapshot snap, const int64 *keys, int nkeys, uint32 *n)
{
	uint32		nk;
	int64	   *sorted = sorted_keys(keys, nkeys, &nk);
	ScopePairs	pairs = scope_pairs(heap, index, scope_ordinal(sift_scope_specs(index), key_name), km, snap, sorted, nk, NULL, 0, 0);

	return pair_values(&pairs, false, n);
}

Oid
sift_scope_pairs_index(Relation heap, Relation index, const char *key_name, Relation *rel, AttrNumber *member, AttrNumber *skey)
{
	PairScan	ps = {0};
	int			ord = scope_ordinal(sift_scope_specs(index), key_name);

	scope_relation(heap, index, ord, NULL, &ps, NULL, 0, NULL);
	*rel = ps.rel;
	*member = ps.member;
	*skey = ps.skey;
	/* the leaves of one journal list the pairs of one relation; the members of a scope of two paths are listed by sift_scope_members, and the leaves hold the strings of a text key, not its codes */
	return sift_scope_specs(index)->s[ord]->joined != NULL || sift_keymap_type(get_atttype(RelationGetRelid(ps.rel), ps.skey)) ? InvalidOid : sift_scope_pairs_journal(ps.rel, ps.member, ps.skey, ps.btree);
}

/* ---- what the segments of the index say ---- */

/*
 * The stamps the scope segments of the scope were filled under, one for each journal of the scope: the oldest of the segments when they differ in the horizon (the keys that changed since
 * it are the keys any of them may be wrong for), or the reason they cannot be read as one: none (unfilled, or filled for other journals than the scope has now), stamps of two journals.
 */
static const char *
segments_stamp(Relation index, const SiftDirSnapshot *dir, int ord, SiftConnStamp *oldest)
{
	int			nj = sift_scope_njournals(sift_scope_specs(index)->s[ord]);
	bool		first = true;

	memset(oldest, 0, SIFT_SCOPE_JOURNALS * sizeof(SiftConnStamp));
	for (int i = 0; i < dir->nsegs; i++)
	{
		SiftColSegment *seg;
		SiftConnStamp st[SIFT_SCOPE_JOURNALS] = {{0}};

		if (!sift_seg_is_scope(&dir->segs[i]))
			continue;
		seg = sift_cseg_open(index, &dir->segs[i], CurrentMemoryContext);
		if (ord < seg->nscope && seg->scope_journals[ord] == nj)
			memcpy(st, seg->scope[ord], nj * sizeof(SiftConnStamp));
		sift_cseg_close(seg);
		for (int j = 0; j < nj; j++)
		{
			if (st[j].lineage == 0)
				return "unfilled";
			if (!first && st[j].lineage != oldest[j].lineage)
				return "several stamps";
		}
		for (int j = 0; j < nj; j++)
			if (first || TransactionIdPrecedes(st[j].xmin, oldest[j].xmin))
				oldest[j] = st[j];
		first = false;
	}
	return first ? "unfilled" : NULL;
}

/*
 * Do the scope segments of the index cover its rows? Every entity segment of the directory says its rows have their entries in them (SIFT_SEG_SCOPED), which the writers keep
 * true: the build and a promotion write the entity segment and the scope segments of its rows together, in one switch of the directory; a merge of entity segments keeps the
 * addresses, so the entries stay valid, and its output has the flag only if all its inputs had it; a merge of scope segments loses none; VACUUM ends the entries of a dead
 * address in every segment. A row not in an entity segment is in a write buffer, which the reader reads through the entity path. The flag is of the index (all its scopes are filled
 * together), the argument checks the ordinal.
 */
bool
sift_scope_covered(Relation index, int scope)
{
	SiftDirSnapshot *dir = palloc(sizeof(SiftDirSnapshot));
	bool		covered = scope >= 0 && scope < sift_col_rel(index)->nscope;

	sift_dir_read(index, dir);
	for (int i = 0; i < dir->nsegs && covered; i++)
		covered = dir->segs[i].kind != SIFT_SEG_IMMUTABLE || sift_seg_is_scope(&dir->segs[i]) || sift_seg_is_scoped(&dir->segs[i]);
	pfree(dir);
	return covered;
}

/*
 * The most changed members worth a patch: each costs its probe, and past the entity rows of the index read whole the entity path is cheaper. A read that only the patch can answer (a row
 * for each pair of several keys: the entity path has one row each) and sift.test_scope_patch take the bound of the journal read.
 */
static uint32
patch_cap(const SiftDirSnapshot *dir, bool entity_path)
{
	uint64		rows = 0;

	for (int i = 0; i < dir->nsegs; i++)
		if (dir->segs[i].kind == SIFT_SEG_IMMUTABLE && !sift_seg_is_scope(&dir->segs[i]))
			rows += dir->segs[i].ndocs;
	return sift_test_scope_patch || !entity_path ? SIFT_SCOPE_CHANGED_MAX : Min(SIFT_SCOPE_CHANGED_MAX, rows * SIFT_SCOPE_CAP_ROW_INSTR / SIFT_SCOPE_MEMBER_INSTR);
}

/* The first pair of each member of ps (ascending by member) */
static ScopePairs
pairs_first(const ScopePairs *ps)
{
	ScopePairs	out = {0};

	for (uint32 i = 0; i < ps->n; i++)
		if (i == 0 || ps->p[i - 1].member != ps->p[i].member)
			pairs_add(&out, ps->p[i].member, ps->p[i].key);
	return out;
}

/* The pairs of ps (ascending by member) of the members m[0..n) */
static ScopePairs
pairs_of(const ScopePairs *ps, const int64 *m, uint32 n)
{
	ScopePairs	out = {0};

	for (uint32 i = 0; i < ps->n; i++)
		if (has_i64(m, n, ps->p[i].member))
			pairs_add(&out, ps->p[i].member, ps->p[i].key);
	return out;
}

/*
 * The facts of a member that has a pair with several of the keys are in the units of each of them, and count once: in those of its least key. The units of the others leave it out:
 * rd->drop[i] lists, for a key, the members. The members of except are read through the entity path and are in no unit read. False: the lists take more than work_mem.
 */
static bool
dedupe_drops(const ScopePairs *all, const int64 *except, uint32 nexcept, SiftScopeRead *rd)
{
	ScopePairs	drops = {0};
	uint32		start = 0;

	for (uint32 i = 1; i <= all->n; i++)
	{
		if (i < all->n && all->p[i].member == all->p[start].member)
			continue;
		if (i - start > 1 && !has_i64(except, nexcept, all->p[start].member))
			for (uint32 j = start + 1; j < i; j++)
				pairs_add(&drops, all->p[start].member, all->p[j].key);
		start = i;
	}
	if ((Size) drops.n * sizeof(SiftScopePair) > (Size) work_mem * 1024)
		return false;
	if (drops.n > 0)
		pairs_by_key(drops.p, drops.n);
	start = 0;
	for (uint32 i = 1; i <= drops.n; i++)
	{
		SiftScopeDrop *d;

		if (i < drops.n && drops.p[i].key == drops.p[start].key)
			continue;
		if (rd->ndrop == SCOPE_DROP_KEYS)
		{
			rd->why = "dedupe keys";
			return true;
		}
		rd->drop = rd->drop ? repalloc(rd->drop, (rd->ndrop + 1) * sizeof(SiftScopeDrop)) : palloc(sizeof(SiftScopeDrop));
		d = &rd->drop[rd->ndrop++];
		d->key = drops.p[start].key;
		d->n = i - start;
		d->members = palloc(d->n * sizeof(int64));
		for (uint32 j = 0; j < d->n; j++)
			d->members[j] = drops.p[start + j].member;
		start = i;
	}
	return true;
}

/* What sift_scope_read_rows needs of the read that planned the rows of the patch */
typedef struct ScopeFill
{
	Relation	heap;
	Relation	index;
	Snapshot	snap;
	int			ord;
	const SiftDirSnapshot *dir;
	SiftScopeBuffer *bufs;
	int			nbufs;
	ScopePairs	delta;			/* the pairs the rows of the buffers and of the patch are copied by */
	SiftScopeSegPlan *plan;
	const int  *leaves;
	int			nleaves;
} ScopeFill;

/* What the second half of the stored read (stored_pairs) needs of the first */
typedef struct ScopeStage
{
	Relation	heap;
	Relation	index;
	Snapshot	snap;
	int			ord;
	const SiftDirSnapshot *dir;
	const int64 *keys;
	uint32		nkeys;
	bool		once;
	bool		topn;
	bool		several;
	const int  *leaves;
	int			nleaves;
	SiftScopeBuffer *bufs;
	int			nbufs;
	uint64		nbuf;
	uint64		nrows;			/* the rows of the relation of pairs */
	double		rows_per_member;
	SiftKeyMap *km;
} ScopeStage;

/*
 * What the stored read of the scope needs besides the units of the keys, as the journal vouches (the rule above), first half: the members to leave out (changed since the stamp, of the pairs of the
 * keys read: the journal tags its records by the scope key), the write buffers, and what they are expected to cost, from the journal alone: the members whose last record inserted a pair are expected to
 * be patched, and their rows are counted from the point directories of the entity segments (the second half lists the pairs). A read that is not to go on (the join costs less) is decided here, before the
 * pairs of the members are asked of the relation, which is most of what the changed members cost. Sets rd->why when the stored read is not exact; rd->stage when the second half is to be done.
 */
static void
stored_changed(Relation heap, Relation index, Snapshot snap, int ord, SiftKeyMap *km, const SiftDirSnapshot *dir, const int64 *keys, uint32 nkeys, bool once, bool topn, const int *leaves, int nleaves,
			   uint32 max_changed, SiftScopeRead *rd)
{
	SiftConnStamp stamp[SIFT_SCOPE_JOURNALS];
	SiftConnKeys changed = {0};
	SiftConnWhy verdict;
	uint32		cap = Min(patch_cap(dir, once || nkeys == 1), max_changed);
	bool		several = once && nkeys > 1;
	SiftScopeBuffer *bufs;
	int			nbufs;
	uint64		nbuf;
	PairScan	ps = {0};
	ScopeStage *st;

	if ((rd->why = segments_stamp(index, dir, ord, stamp)) != NULL)
	{
		if (!sift_scope_has_journal(index, ord))
			rd->why = sift_conn_why_name(SIFT_CONN_NO_JOURNAL);
		return;
	}
	if (!sift_scope_covered(index, ord))
	{
		rd->why = "scope segments do not cover the rows";
		return;
	}
	if ((verdict = sift_scope_changed(index, ord, stamp, snap, cap, keys, nkeys, &changed)) != SIFT_CONN_OK)
	{
		rd->why = sift_conn_why_name(verdict);
		return;
	}
	rd->nchanged = rd->nexcept = changed.n;
	rd->except = palloc(Max(changed.n, 1) * sizeof(int64));
	rd->added = palloc(Max(changed.n, 1) * sizeof(int64));
	for (uint32 i = 0; i < changed.n; i++)
	{
		rd->except[i] = changed.keys[i];
		if (changed.added[i])
			rd->added[rd->nadded++] = changed.keys[i];
	}
	sift_conn_keys_free(&changed);
	bufs = sift_scope_buffers(index, dir, &nbufs);
	rd->buffer_rows = nbuf = sift_scope_buffers_rows(bufs, nbufs);
	if (rd->nexcept == 0 && nbuf == 0 && !several)
		return;
	scope_relation(heap, index, ord, km, &ps, keys, nkeys, snap);
	st = rd->stage = palloc(sizeof(ScopeStage));
	*st = (ScopeStage) {heap, index, snap, ord, dir, keys, nkeys, once, topn, several, leaves, nleaves, bufs, nbufs, nbuf, (uint64) sift_conn_rows_of(ps.rel), 0, km};
	table_close(ps.rel, NoLock);
	for (int i = 0; i < dir->nsegs; i++)
		if (dir->segs[i].kind == SIFT_SEG_IMMUTABLE && !sift_seg_is_scope(&dir->segs[i]))
			rd->entity_rows += dir->segs[i].ndocs;
	/* the rows a member has in the entity segments, from the statistics of its column; the rows each pair of the relation stands for when ANALYZE has none */
	st->rows_per_member = sift_col_leaf_rows_per_value(index, &sift_col_rel(index)->schema->cols[sift_col_rel(index)->scope_leaf[ord]], rd->entity_rows);
	if (st->rows_per_member == 0)
		st->rows_per_member = rd->entity_rows / Max((double) st->nrows, 1.0);
	if (rd->nadded > 0)
	{
		rd->est_plan = sift_scope_segment_plan(index, ord, dir, rd->added, rd->nadded, st->rows_per_member, &rd->patch_rows, &rd->patch_scan, &rd->patch_units);
		rd->delta_rows = nbuf;
	}
}

/* the second half: the pairs of the members, the patch, the drops, the delta */
static void
stored_pairs(SiftScopeRead *rd)
{
	ScopeStage *st = rd->stage;
	uint32		max = 0;
	ScopePairs	all,
				patch,
				delta;

	rd->stage = NULL;
	if (st->several || st->nbuf > 0)
		max = (work_mem * 1024L) / sizeof(SiftScopePair);
	if (st->several || (st->nbuf > 0 && !sift_conn_probe_cheaper(st->nbuf, st->nrows)))
		all = scope_pairs(st->heap, st->index, st->ord, st->km, st->snap, st->keys, st->nkeys, NULL, 0, max);	/* the buffers hold more rows than the pairs of the keys are worth listing members for */
	else
	{
		uint32		nmem = 0,
					nboth;
		int64	   *members = st->nbuf > 0 ? sift_scope_buffers_members(st->index, st->ord, st->bufs, st->nbufs, &nmem) : NULL;
		int64	   *both = union_i64(rd->except, rd->nexcept, members, nmem, &nboth);

		all = scope_pairs(st->heap, st->index, st->ord, st->km, st->snap, st->keys, st->nkeys, both, nboth, max);
	}
	rd->pairs = all.n;
	if (all.over)
		rd->why = st->several ? "dedupe over work_mem" : "write buffer";
	else if (st->several && !dedupe_drops(&all, rd->except, rd->nexcept, rd))
		rd->why = "dedupe over work_mem";
	if (rd->why != NULL)
		return;
	patch = pairs_of(&all, rd->except, rd->nexcept);
	rd->patch = palloc(Max(patch.n, 1) * sizeof(int64));
	for (uint32 i = 0; i < patch.n; i++)
		rd->patch[i] = patch.p[i].member;
	rd->npatch = sort_unique(rd->patch, patch.n);
	/* a member of several keys counts once, in its least key, for the rows that have no entry yet as for those that have */
	delta = st->several ? pairs_first(&all) : all;
	rd->delta = sift_scope_delta_select(st->index, st->ord, st->bufs, st->nbufs, delta.p, delta.n, st->leaves, st->nleaves, &rd->delta_rows);
	if ((rd->npatch > 0 || rd->delta_rows > 0) && st->topn)
		rd->why = "top-n, changed pairs";
	if (rd->why != NULL)
		return;
	rd->fill = palloc(sizeof(ScopeFill));
	*rd->fill = (ScopeFill) {st->heap, st->index, st->snap, st->ord, st->dir, st->bufs, st->nbufs, delta, NULL, st->leaves, st->nleaves};
	if (rd->npatch > 0)
	{
		/* the members expected from the journal are those the pairs give: the plan made for them is the one */
		if (rd->est_plan != NULL && rd->npatch == rd->nadded && memcmp(rd->patch, rd->added, rd->npatch * sizeof(int64)) == 0)
			rd->fill->plan = rd->est_plan;
		else
			rd->fill->plan = sift_scope_segment_plan(st->index, st->ord, st->dir, rd->patch, rd->npatch, st->rows_per_member, &rd->patch_rows, &rd->patch_scan, &rd->patch_units);
	}
	else
		rd->patch_rows = rd->patch_scan = rd->patch_units = 0;
}

void
sift_scope_read_pairs(SiftScopeRead *rd)
{
	if (rd->stage != NULL)
		stored_pairs(rd);
}

void
sift_scope_read_rows(SiftScopeRead *rd)
{
	ScopeFill  *f = rd->fill;
	SiftScopeBuffer *segs;
	int			nsegs;
	struct SiftScopeDelta *d;
	uint64		copies;

	if (f == NULL || f->plan == NULL)
		return;
	nsegs = f->nbufs;
	segs = sift_scope_segment_rows(f->plan, f->heap, f->index, f->snap, f->ord, f->dir, f->bufs, &nsegs, f->leaves, f->nleaves, &rd->patch_rows);
	d = sift_scope_delta_select(f->index, f->ord, segs + f->nbufs, nsegs - f->nbufs, f->delta.p, f->delta.n, f->leaves, f->nleaves, &copies);
	rd->delta = sift_scope_delta_merge(rd->delta, d);
	rd->delta_rows += copies;
}

/* the key most of the pairs of ps have (the least of them when several do) */
static int64
commonest_key(const ScopePairs *ps)
{
	int64	   *k = palloc(ps->n * sizeof(int64));
	int64		key = 0;
	uint32		best = 0,
				run = 0;

	for (uint32 i = 0; i < ps->n; i++)
		k[i] = ps->p[i].key;
	sift_sort_i64(k, ps->n);
	for (uint32 i = 0; i < ps->n; i++)
	{
		run = i > 0 && k[i] == k[i - 1] ? run + 1 : 1;
		if (run > best)
		{
			best = run;
			key = k[i];
		}
	}
	pfree(k);
	return key;
}

bool
sift_scope_patch_counts(Relation heap, Relation index, Snapshot snap, int scope, SiftScopeCounts *c)
{
	SiftDirSnapshot *dir = palloc(sizeof(SiftDirSnapshot));
	SiftConnStamp stamp[SIFT_SCOPE_JOURNALS];
	SiftConnKeys changed = {0};
	ScopePairs	pairs;
	SiftScopeRead *rd;
	SiftKeyMap *km;
	int64		key;

	*c = (SiftScopeCounts) {.nkeys = 1,.once = true};
	sift_dir_read(index, dir);
	if (segments_stamp(index, dir, scope, stamp) != NULL || sift_scope_vouch(index, scope, stamp, snap) != SIFT_CONN_OK ||
		sift_scope_changed(index, scope, stamp, snap, SIFT_SCOPE_CHANGED_MAX, NULL, 0, &changed) != SIFT_CONN_OK)
		return false;
	c->changed = changed.n;
	km = sift_scope_keymap(heap, index, sift_scope_specs(index)->s[scope]->key_name);
	pairs = scope_pairs(heap, index, scope, km, snap, NULL, 0, changed.keys, changed.n, 0);
	sift_conn_keys_free(&changed);
	if (pairs.n == 0)
		return true;	/* no changed member has a pair now: nothing to patch */
	key = commonest_key(&pairs);
	rd = sift_scope_read(heap, index, snap, sift_scope_specs(index)->s[scope]->key_name, km, &key, 1, true, false, NULL, 0, PG_UINT32_MAX, false);
	if (rd->why != NULL)
		return false;
	c->patch_units = rd->patch_units;
	c->patch_scan_rows = rd->patch_scan;
	c->delta_rows = rd->patch_rows;
	return true;
}

SiftScopeRead *
sift_scope_read(Relation heap, Relation index, Snapshot snap, const char *key_name, SiftKeyMap *km, const int64 *keys, int nkeys, bool once, bool topn, const int *leaves, int nleaves,
				uint32 max_changed, bool lazy)
{
	SiftScopeRead *rd = palloc0(sizeof(SiftScopeRead));
	SiftDirSnapshot *dir;
	uint32		nk;
	int64	   *sorted;
	int			ord = scope_ordinal(sift_scope_specs(index), key_name);
	const char *missing;

	if (!sift_agg_scope)
		rd->why = "disabled";
	else if (ord < 0)
		rd->why = "no scope index";
	else if ((missing = unit_missing(index, ord, leaves, nleaves)) != NULL)
		rd->why = psprintf("column %s not in the scope units", missing);
	if (rd->why != NULL)
		return rd;
	rd->scope = ord;
	SIFT_INVARIANT(keys != NULL || !once, "sift: a scope read of every key counts a row for each pair");
	sorted = sorted_keys(keys, nkeys, &nk);
	(void) sift_cwbuf_flush(index);
	dir = palloc(sizeof(SiftDirSnapshot));
	sift_dir_read(index, dir);
	rd->view = sift_col_scope_view(heap, dir);
	SIFT_COL_OPEN_GATE();
	stored_changed(heap, index, snap, ord, km, dir, sorted, nk, once, topn, leaves, nleaves, max_changed, rd);
	if (!lazy)
		sift_scope_read_pairs(rd);
	return rd;
}
