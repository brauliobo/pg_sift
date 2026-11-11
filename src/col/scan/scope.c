/*
 * col/scan/scope.c - what the reader of a scope index asks of the index and of its scope relation (col/scope.h)
 *
 * A scan that has a scope conjunct reads the scope units of its keys, less the members that changed in the scope relation since the stamp of the units, plus the entity rows of
 * those of them that have a pair with a key now (the patch), plus the rows of the write buffers (no scope entry describes them) expanded by the pairs of their members (the delta): INTERFACE section 6,
 * docs/COL-CONNECT.md "The rule". A fact of a member that has a pair with several of the keys counts once for IN and EXISTS (section 6b): the units of all but the least of its keys leave it out.
 * Whenever the stored read is not exact the scan reads the entity rows of the members the scope relation lists for the keys, and says why.
 */
/* check-memory: allow-allocs the arrays follow the keys of one scope conjunct, the pairs of the keys it names and the members the caps of the statement allow, not the rows of the table */
#include "postgres.h"

#include "access/genam.h"
#include "access/table.h"
#include "access/tableam.h"
#include "access/transam.h"
#include "common/int.h"
#include "core/compat.h"
#include "executor/tuptable.h"
#include "optimizer/optimizer.h"
#include "miscadmin.h"
#include "utils/memutils.h"
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

static int
cmp_by_member(const void *a, const void *b)
{
	const SiftScopePair *x = a,
			   *y = b;

	return x->member != y->member ? pg_cmp_s64(x->member, y->member) : pg_cmp_s64(x->key, y->key);
}

static int
cmp_by_key(const void *a, const void *b)
{
	const SiftScopePair *x = a,
			   *y = b;

	return x->key != y->key ? pg_cmp_s64(x->key, y->key) : pg_cmp_s64(x->member, y->member);
}

/* the distinct values of a, ascending: sorts in place and returns how many */
static uint32
sort_unique(int64 *a, uint32 n)
{
	uint32		k = 0;

	if (n > 0)
		qsort(a, n, sizeof(int64), cmp_i64);
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

/* ---- the pairs of the scope relation under the snapshot ---- */

typedef struct PairScan
{
	Relation	rel;
	Oid			btree;
	AttrNumber	member;
	AttrNumber	skey;
	Snapshot	snap;
	const int64 *keys;			/* the scope keys asked for, sorted and distinct; NULL: every key */
	uint32		nkeys;
	ScopePairs	out;
	int64		cur;			/* the key or the member a probe asks for */
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
	int64		k;

	if (sift_conn_slot_key(slot, ps->skey, &k) && wanted(ps, k))
		pairs_add(&ps->out, ps->cur, k);
	return true;
}

/* The btree asked for each value of the column it leads with (probe of key or of member), each hit handed to each */
static void
probe_all(PairScan *ps, AttrNumber lead, const int64 *vals, uint32 n, SiftConnProbeFn each)
{
	SiftConnProbe *p = sift_conn_probe_begin_on(CurrentMemoryContext, ps->rel, ps->btree, lead, ps->snap);
	SiftConnProbeScan s;

	sift_conn_probe_open(p, &s);
	for (uint32 i = 0; i < n; i++)
	{
		ps->cur = vals[i];
		sift_conn_probe_key(p, &s, ps->cur, each, ps);
	}
	sift_conn_probe_close(&s);
}

/* A pair read from the leaves of the journal index of the relation costs about this (the heap read of FILL_SCAN_NS_PER_PARENT_ROW in conn_fill.c is 13 times that), a probe of the btree about 3,500 */
#define LEAF_PAIR_NS	10
#define PROBE_NS		3500

static bool
leaf_pairs(PairScan *ps)
{
	Oid			journal = sift_scope_pairs_journal(ps->rel, ps->member, ps->skey, ps->btree);
	SiftScopePair *p;
	uint32		n;

	if (!OidIsValid(journal) || !lookcol_pairs(ps->rel, journal, ps->snap, ps->member, ps->skey, ps->keys, ps->nkeys, &p, &n))
		return false;
	for (uint32 i = 0; i < n; i++)
		pairs_add(&ps->out, p[i].member, p[i].key);
	return true;
}

/*
 * The pairs with one of the keys (any key: keys NULL), ascending by member: the btree asked for each key when it leads with the scope key and the keys are few, else the leaves of the relation's journal index
 * (a sift_col index of both columns; a pair with a NULL in a column is no pair), else the relation read once
 */
static void
pairs_of_keys(PairScan *ps)
{
	if (ps->keys != NULL && btree_leads_with(ps->btree, ps->skey) && (double) ps->nkeys * PROBE_NS < sift_conn_rows_of(ps->rel) * LEAF_PAIR_NS)
		probe_all(ps, ps->skey, ps->keys, ps->nkeys, probe_of_key);
	else if (!leaf_pairs(ps))
		sift_conn_scan_keys(ps->rel, ps->btree, ps->member, ps->skey, ps->snap, scan_pair, ps);
	if (ps->out.n > 0)
		qsort(ps->out.p, ps->out.n, sizeof(SiftScopePair), cmp_by_member);
}

/* The pairs of the members with one of the keys: the btree asked for each member when it leads with the member and that is cheaper than reading the pairs of the keys (the fill's price rule), else the pairs of the keys filtered */
static void
pairs_of_members(PairScan *ps, const int64 *members, uint32 n)
{
	uint32		k = 0;

	if (btree_leads_with(ps->btree, ps->member) && sift_conn_probe_cheaper(n, sift_conn_rows_of(ps->rel)))
	{
		probe_all(ps, ps->member, members, n, probe_of_member);
		if (ps->out.n > 0)
			qsort(ps->out.p, ps->out.n, sizeof(SiftScopePair), cmp_by_member);
		return;
	}
	pairs_of_keys(ps);
	for (uint32 i = 0; i < ps->out.n; i++)
		if (has_i64(members, n, ps->out.p[i].member))
			ps->out.p[k++] = ps->out.p[i];
	ps->out.n = k;
}

/* The scope relation and its btree of a declared scope; the relation is open (AccessShare) */
static Oid
scope_relation(Relation heap, Relation index, int ord, PairScan *ps, const int64 *keys, uint32 nkeys, Snapshot snap)
{
	SiftScopeSpecs *sp = sift_scope_specs(index);
	Oid			btree = sift_scope_relation_index(heap, index, sp->s[ord], &ps->rel, &ps->member, &ps->skey);

	ps->btree = btree;
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

/* Sorts the pairs by member and keeps each once */
static void
pairs_distinct(ScopePairs *ps)
{
	uint32		k = 0;

	if (ps->n > 0)
		qsort(ps->p, ps->n, sizeof(SiftScopePair), cmp_by_member);
	for (uint32 i = 0; i < ps->n; i++)
		if (k == 0 || cmp_by_member(&ps->p[k - 1], &ps->p[i]) != 0)
			ps->p[k++] = ps->p[i];
	ps->n = k;
}

/*
 * The distinct pairs of the scope under snap with one of the keys, ascending by member: those of the members only (members NULL: every member's). At most max of them are held (0: any; over is set
 * when there are more). Of two paths the pairs are those of the pairs relation and of the joined path.
 */
static ScopePairs
scope_pairs(Relation heap, Relation index, int ord, Snapshot snap, const int64 *keys, uint32 nkeys, const int64 *members, uint32 nmembers, uint32 max)
{
	const SiftScopeSpec *s = sift_scope_specs(index)->s[ord];
	PairScan	ps = {0};

	scope_relation(heap, index, ord, &ps, keys, nkeys, snap);
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

int64 *
sift_scope_members(Relation heap, Relation index, const char *key_name, Snapshot snap, const int64 *keys, int nkeys, uint32 *n)
{
	uint32		nk;
	int64	   *sorted = sorted_keys(keys, nkeys, &nk);
	ScopePairs	pairs = scope_pairs(heap, index, scope_ordinal(sift_scope_specs(index), key_name), snap, sorted, nk, NULL, 0, 0);

	return pair_values(&pairs, false, n);
}

Oid
sift_scope_pairs_index(Relation heap, Relation index, const char *key_name, Relation *rel, AttrNumber *member, AttrNumber *skey)
{
	PairScan	ps = {0};
	int			ord = scope_ordinal(sift_scope_specs(index), key_name);

	scope_relation(heap, index, ord, &ps, NULL, 0, NULL);
	*rel = ps.rel;
	*member = ps.member;
	*skey = ps.skey;
	/* the leaves of one journal list the pairs of one relation; the members of a scope of two paths are listed by sift_scope_members */
	return sift_scope_specs(index)->s[ord]->joined != NULL ? InvalidOid : sift_scope_pairs_journal(ps.rel, ps.member, ps.skey, ps.btree);
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
	return sift_test_scope_patch || !entity_path ? SIFT_SCOPE_CHANGED_MAX : Min(SIFT_SCOPE_CHANGED_MAX, rows * SIFT_SCOPE_ENTITY_INSTR / SIFT_SCOPE_MEMBER_INSTR);
}

/* The pairs of ps (ascending by member) of the members not in skip, only the first of each member when first */
static ScopePairs
pairs_not_in(const ScopePairs *ps, const int64 *skip, uint32 nskip, bool first)
{
	ScopePairs	out = {0};

	for (uint32 i = 0; i < ps->n; i++)
		if (!has_i64(skip, nskip, ps->p[i].member) && !(first && i > 0 && ps->p[i - 1].member == ps->p[i].member))
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
		qsort(drops.p, drops.n, sizeof(SiftScopePair), cmp_by_key);
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

/* Does any member of the pairs (ascending by member) have a pair with two of the keys */
static bool
any_member_twice(const ScopePairs *ps)
{
	for (uint32 i = 1; i < ps->n; i++)
		if (ps->p[i].member == ps->p[i - 1].member)
			return true;
	return false;
}

/*
 * What the stored read of the scope needs besides the units of the keys, as the journal vouches (the rule above): the members to leave out (changed since the stamp), those of them
 * with a pair now (the patch), the drops of the facts in several keys, and the pairs the rows of the write buffers are expanded by (the delta: no scope entry describes them, and
 * the stored entries of their members are valid unless the member changed). Sets rd->why when the stored read is not exact.
 */
static void
stored_read(Relation heap, Relation index, Snapshot snap, int ord, const SiftDirSnapshot *dir, const int64 *keys, uint32 nkeys, bool once, bool topn, const int *leaves, int nleaves, SiftScopeRead *rd)
{
	SiftConnStamp stamp[SIFT_SCOPE_JOURNALS];
	SiftConnKeys changed = {0};
	SiftConnWhy verdict;
	uint32		cap = patch_cap(dir, once || nkeys == 1);
	bool		several = once && nkeys > 1;
	SiftScopeBuffer *bufs;
	int			nbufs;
	uint64		nbuf;
	PairScan	ps = {0};
	uint64		nrows;
	uint32		max = 0;
	ScopePairs	all,
				patch,
				delta;

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
	if ((verdict = sift_scope_vouch(index, ord, stamp, snap)) != SIFT_CONN_OK ||
		(verdict = sift_scope_changed(index, ord, stamp, snap, cap, &changed)) != SIFT_CONN_OK)
	{
		rd->why = sift_conn_why_name(verdict);
		return;
	}
	rd->nchanged = rd->nexcept = changed.n;
	rd->except = palloc(Max(changed.n, 1) * sizeof(int64));
	memcpy(rd->except, changed.keys, changed.n * sizeof(int64));
	sift_conn_keys_free(&changed);
	bufs = sift_scope_buffers(index, dir, &nbufs);
	rd->buffer_rows = nbuf = sift_scope_buffers_rows(bufs, nbufs);
	if (rd->nexcept == 0 && nbuf == 0 && !several)
		return;
	scope_relation(heap, index, ord, &ps, keys, nkeys, snap);
	nrows = sift_conn_rows_of(ps.rel);
	table_close(ps.rel, NoLock);
	if (several || nbuf > 0)
		max = (work_mem * 1024L) / sizeof(SiftScopePair);
	if (several || (nbuf > 0 && !sift_conn_probe_cheaper(nbuf, nrows)))
		all = scope_pairs(heap, index, ord, snap, keys, nkeys, NULL, 0, max);	/* the buffers hold more rows than the pairs of the keys are worth listing members for */
	else
	{
		uint32		nmem = 0,
					nboth;
		int64	   *members = nbuf > 0 ? sift_scope_buffers_members(index, ord, bufs, nbufs, &nmem) : NULL;
		int64	   *both = union_i64(rd->except, rd->nexcept, members, nmem, &nboth);

		all = scope_pairs(heap, index, ord, snap, keys, nkeys, both, nboth, max);
	}
	if (all.over)
		rd->why = several ? "dedupe over work_mem" : "write buffer";
	else if (several && !dedupe_drops(&all, rd->except, rd->nexcept, rd))
		rd->why = "dedupe over work_mem";
	if (rd->why != NULL)
		return;
	patch = pairs_of(&all, rd->except, rd->nexcept);
	/* the rows of a changed member are patched with the rest of its rows; a member of several keys counts once, in its least key, for the rows that have no entry yet as for those that have */
	delta = nbuf > 0 ? pairs_not_in(&all, rd->except, rd->nexcept, several) : (ScopePairs) {0};
	rd->delta = sift_scope_delta_select(index, ord, bufs, nbufs, delta.p, delta.n, leaves, nleaves, &rd->delta_rows);
	rd->patch = palloc(Max(patch.n, 1) * sizeof(int64));
	for (uint32 i = 0; i < patch.n; i++)
		rd->patch[i] = patch.p[i].member;
	rd->npatch = sort_unique(rd->patch, patch.n);
	/* the entity rows of a member are one fact each, with no scope key: a row that counts once for each of the member's pairs with the keys, or that gives its key, can only come from the units */
	if (rd->npatch > 0 && !once && (keys == NULL || any_member_twice(&patch)))
		rd->why = "per pair, changed pairs";
	else if ((rd->npatch > 0 || rd->delta_rows > 0) && topn)
		rd->why = "top-n, changed pairs";
}

SiftScopeRead *
sift_scope_read(Relation heap, Relation index, Snapshot snap, const char *key_name, const int64 *keys, int nkeys, bool once, bool topn, const int *leaves, int nleaves)
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
	stored_read(heap, index, snap, ord, dir, sorted, nk, once, topn, leaves, nleaves, rd);
	pfree(dir);
	return rd;
}
