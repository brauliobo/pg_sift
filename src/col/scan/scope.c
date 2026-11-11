/*
 * col/scan/scope.c - what the reader of a scope index asks of the index and of its scope relation (col/scope.h)
 *
 * A scan that has a scope conjunct reads the scope units of its keys, less the members that changed in the scope relation since the stamp of the units and the members that
 * have a row in a write buffer (no scope entry describes it), plus the entity rows of those of them that have a pair with a key now (the patch): INTERFACE section 6,
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
#include "miscadmin.h"
#include "utils/memutils.h"
#include "utils/rel.h"

#include "col/am.h"
#include "col/seg.h"
#include "col/scope.h"
#include "col/wbuf.h"

/* a key whose units leave members out is one conjunct of the scan: the keys of an IN list that have them are at most this many */
#define SCOPE_DROP_KEYS 64

typedef struct ScopePair
{
	int64		member;
	int64		key;
} ScopePair;

typedef struct ScopePairs
{
	ScopePair  *p;
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
	const ScopePair *x = a,
			   *y = b;

	return x->member != y->member ? pg_cmp_s64(x->member, y->member) : pg_cmp_s64(x->key, y->key);
}

static int
cmp_by_key(const void *a, const void *b)
{
	const ScopePair *x = a,
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
	*n = sort_unique(u, na + nb);
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
		ps->p = ps->p ? repalloc(ps->p, ps->cap * sizeof(ScopePair)) : palloc(ps->cap * sizeof(ScopePair));
	}
	ps->p[ps->n++] = (ScopePair) {member, key};
}

/* ---- the pairs of the scope relation under the snapshot ---- */

typedef struct PairScan
{
	Relation	rel;
	Oid			btree;
	AttrNumber	member;
	AttrNumber	skey;
	Snapshot	snap;
	const int64 *keys;			/* the scope keys asked for, sorted and distinct */
	uint32		nkeys;
	ScopePairs	out;
	int64		cur;			/* the key or the member a probe asks for */
} PairScan;

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

	if (has_i64(ps->keys, ps->nkeys, key))
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

	if (sift_conn_slot_key(slot, ps->skey, &k) && has_i64(ps->keys, ps->nkeys, k))
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

/* The pairs with one of the keys, ascending by member: the btree asked for each key when it leads with the scope key, else the relation read once */
static void
pairs_of_keys(PairScan *ps)
{
	if (btree_leads_with(ps->btree, ps->skey))
		probe_all(ps, ps->skey, ps->keys, ps->nkeys, probe_of_key);
	else
		sift_conn_scan_keys(ps->rel, ps->btree, ps->member, ps->skey, ps->snap, scan_pair, ps);
	if (ps->out.n > 0)
		qsort(ps->out.p, ps->out.n, sizeof(ScopePair), cmp_by_member);
}

/* The pairs of the members with one of the keys: the btree asked for each member when it leads with the member (the members are few, the keys' may be many), else the pairs of the keys filtered */
static void
pairs_of_members(PairScan *ps, const int64 *members, uint32 n)
{
	uint32		k = 0;

	if (btree_leads_with(ps->btree, ps->member))
	{
		probe_all(ps, ps->member, members, n, probe_of_member);
		if (ps->out.n > 0)
			qsort(ps->out.p, ps->out.n, sizeof(ScopePair), cmp_by_member);
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

int64 *
sift_scope_members(Relation heap, Relation index, const char *key_name, Snapshot snap, const int64 *keys, int nkeys, uint32 *n)
{
	int64	   *sorted = palloc(Max(nkeys, 1) * sizeof(int64)),
			   *out;
	PairScan	ps = {0};
	uint32		nk;

	memcpy(sorted, keys, nkeys * sizeof(int64));
	nk = sort_unique(sorted, nkeys);
	scope_relation(heap, index, scope_ordinal(sift_scope_specs(index), key_name), &ps, sorted, nk, snap);
	pairs_of_keys(&ps);
	table_close(ps.rel, NoLock);
	out = palloc(Max(ps.out.n, 1) * sizeof(int64));
	for (uint32 i = 0; i < ps.out.n; i++)
		out[i] = ps.out.p[i].member;
	*n = sort_unique(out, ps.out.n);
	return out;
}

Oid
sift_scope_pairs_index(Relation heap, Relation index, const char *key_name, Relation *rel, AttrNumber *member, AttrNumber *skey)
{
	PairScan	ps = {0};

	scope_relation(heap, index, scope_ordinal(sift_scope_specs(index), key_name), &ps, NULL, 0, NULL);
	*rel = ps.rel;
	*member = ps.member;
	*skey = ps.skey;
	return sift_scope_pairs_journal(ps.rel, ps.member, ps.skey, ps.btree);
}

/* ---- what the segments of the index say ---- */

/*
 * The stamp the scope segments of the scope were filled under, the oldest when they differ in the horizon (the keys that changed since it are the keys any of them may be wrong for), or the
 * reason they cannot be read as one: none (unfilled), stamps of two journals.
 */
static const char *
segments_stamp(Relation index, const SiftDirSnapshot *dir, int ord, SiftConnStamp *oldest)
{
	bool		first = true;

	*oldest = (SiftConnStamp) {0};
	for (int i = 0; i < dir->nsegs; i++)
	{
		SiftColSegment *seg;
		SiftConnStamp st;

		if (!sift_seg_is_scope(&dir->segs[i]))
			continue;
		seg = sift_cseg_open(index, &dir->segs[i], CurrentMemoryContext);
		st = ord < seg->nscope ? seg->scope[ord] : (SiftConnStamp) {0};
		sift_cseg_close(seg);
		if (st.lineage == 0)
			return "unfilled";
		if (!first && st.lineage != oldest->lineage)
			return "several stamps";
		if (first || TransactionIdPrecedes(st.xmin, oldest->xmin))
			*oldest = st;
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

/* The distinct member keys of the live records of the write buffers, which no scope entry describes; false when they are more than cap */
static bool
wbuf_members(Relation index, const SiftDirSnapshot *dir, const SiftColSchema *schema, int fk_leaf, uint32 cap, int64 **out, uint32 *n)
{
	MemoryContext cx = AllocSetContextCreate(CurrentMemoryContext, "sift scope write buffer", ALLOCSET_DEFAULT_SIZES);
	int64	   *m = palloc(64 * sizeof(int64));
	uint32		nm = 0,
				space = 64;
	bool		ok = true;

	for (int i = 0; i < dir->nsegs && ok; i++)
	{
		SiftCWbufReader *r;
		SiftColRow	row;
		bool		deleted;

		if (!sift_seg_is_wbuf(&dir->segs[i]))
			continue;
		r = sift_cwbuf_open(index, &dir->segs[i], schema, cx);
		while (ok && sift_cwbuf_next(r, &row, &deleted))
		{
			if (deleted || row.val[fk_leaf].isnull)
				continue;
			if (nm == space)
			{
				/* a bulk insert repeats its members: compact before growing, and count the distinct ones against the cap */
				nm = sort_unique(m, nm);
				if (nm > cap)
					ok = false;
				else if (nm * 2 > space)
					m = repalloc(m, (space *= 2) * sizeof(int64));
			}
			m[nm++] = row.val[fk_leaf].v.i;
			CHECK_FOR_INTERRUPTS();
		}
		sift_cwbuf_close(r);
	}
	MemoryContextDelete(cx);
	*n = ok ? sort_unique(m, nm) : 0;
	*out = m;
	return ok && *n <= cap;
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
	if ((Size) drops.n * sizeof(ScopePair) > (Size) work_mem * 1024)
		return false;
	if (drops.n > 0)
		qsort(drops.p, drops.n, sizeof(ScopePair), cmp_by_key);
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
 * What the stored read of the scope needs besides the units of the keys, as the journal vouches (the rule above): the members to leave out (changed since the stamp, or in a write
 * buffer), those of them with a pair now, and the drops of the facts in several keys. Sets rd->why when the stored read is not exact.
 */
static void
stored_read(Relation heap, Relation index, Snapshot snap, int ord, const SiftDirSnapshot *dir, const int64 *keys, uint32 nkeys, bool once, bool topn, SiftScopeRead *rd)
{
	SiftConnStamp stamp;
	SiftConnKeys changed = {0};
	SiftConnWhy verdict;
	const SiftColRel *r = sift_col_rel_private(index, CurrentMemoryContext);
	uint32		cap = patch_cap(dir, once || nkeys == 1);
	int64	   *wbuf;
	uint32		nwbuf;
	PairScan	ps = {0};
	ScopePairs	all;

	if ((rd->why = segments_stamp(index, dir, ord, &stamp)) != NULL)
		return;
	if (!sift_scope_covered(index, ord))
	{
		rd->why = "scope segments do not cover the rows";
		return;
	}
	if ((verdict = sift_scope_vouch(index, ord, &stamp, snap)) != SIFT_CONN_OK ||
		(verdict = sift_scope_changed(index, ord, &stamp, snap, cap, &changed)) != SIFT_CONN_OK)
	{
		rd->why = sift_conn_why_name(verdict);
		return;
	}
	if (!wbuf_members(index, dir, r->schema, r->first[r->schema->cols[r->scope_leaf[ord]].attno], cap, &wbuf, &nwbuf))
	{
		rd->why = "write buffer";
		sift_conn_keys_free(&changed);
		return;
	}
	rd->nchanged = changed.n;
	rd->except = union_i64(changed.keys, changed.n, wbuf, nwbuf, &rd->nexcept);
	sift_conn_keys_free(&changed);
	if (rd->nexcept == 0 && !(once && nkeys > 1))
		return;
	scope_relation(heap, index, ord, &ps, keys, nkeys, snap);
	if (once && nkeys > 1)
	{
		ps.out.max = (work_mem * 1024L) / sizeof(ScopePair);
		pairs_of_keys(&ps);
		all = ps.out;
		if (all.over || !dedupe_drops(&all, rd->except, rd->nexcept, rd))
			rd->why = "dedupe over work_mem";
		ps.out = (ScopePairs) {0};
		for (uint32 i = 0; rd->why == NULL && i < all.n; i++)
			if (has_i64(rd->except, rd->nexcept, all.p[i].member))
				pairs_add(&ps.out, all.p[i].member, all.p[i].key);
	}
	else
		pairs_of_members(&ps, rd->except, rd->nexcept);
	table_close(ps.rel, NoLock);
	if (rd->why != NULL)
		return;
	rd->patch = palloc(Max(ps.out.n, 1) * sizeof(int64));
	for (uint32 i = 0; i < ps.out.n; i++)
		rd->patch[i] = ps.out.p[i].member;
	rd->npatch = sort_unique(rd->patch, ps.out.n);
	/* the entity rows of a member are one fact each: a row that counts once for each of the member's pairs with the keys can only come from the units */
	if (rd->npatch > 0 && !once && any_member_twice(&ps.out))
		rd->why = "per pair, changed pairs";
	else if (rd->npatch > 0 && topn)
		rd->why = "top-n, changed pairs";
}

SiftScopeRead *
sift_scope_read(Relation heap, Relation index, Snapshot snap, const char *key_name, const int64 *keys, int nkeys, bool once, bool topn, const int *leaves, int nleaves)
{
	SiftScopeRead *rd = palloc0(sizeof(SiftScopeRead));
	SiftDirSnapshot *dir;
	int64	   *sorted;
	uint32		nk;
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
	sorted = palloc(Max(nkeys, 1) * sizeof(int64));
	memcpy(sorted, keys, nkeys * sizeof(int64));
	nk = sort_unique(sorted, nkeys);
	(void) sift_cwbuf_flush(index);
	dir = palloc(sizeof(SiftDirSnapshot));
	sift_dir_read(index, dir);
	stored_read(heap, index, snap, ord, dir, sorted, nk, once, topn, rd);
	pfree(dir);
	return rd;
}
