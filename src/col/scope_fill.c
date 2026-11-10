/*
 * col/scope_fill.c - the fill of a scope index (col/scope.h): the scope keys of the member keys of a batch of fact rows, under one stamp of the scope relation
 *
 * For each scope the fill knows the pairs (member, scope key) of the relation under a snapshot the way the fill of the found flags knows a parent (col/conn_fill.c): by reading the relation
 * once into the pairs sorted by member, or by asking its unique btree for the members of each batch, whichever the rows to fill make cheaper (the same price rule and the same btree probe). The stamp is taken before the
 * relation is read, from the journal of the relation (col/scope_journal.c): lineage 0 (and the snapshot's xmin) when the relation has no journal that vouches for the snapshot.
 */
#include "postgres.h"

#include "access/relation.h"
#include "access/table.h"
#include "access/xact.h"
#include "miscadmin.h"
#include "utils/hsearch.h"
#include "utils/snapmgr.h"

#include "col/am.h"
#include "col/scope.h"

typedef struct SiftScopeFill SiftScopeFill;

/* The members asked are remembered up to this many (or this many of their keys), then forgotten at the next batch */
#define SEEN_MAX	(1 << 18)
#define POOL_MAX	((uint64) 1 << 22)

typedef struct ScopePair
{
	int64		member;
	int64		key;
} ScopePair;

/* A probe's answer for a member: its keys are pool[start, start + n) */
typedef struct ScopeSeen
{
	int64		member;
	uint32		start;
	uint32		n;
} ScopeSeen;

typedef struct ScopeMap
{
	SiftConnStamp stamp;
	int			fk_leaf;		/* the leaf of the fact index that holds the member key */
	ScopePair  *pairs;			/* the key set: every pair, sorted (NULL: the probe) */
	uint64		npairs;
	MemoryContext cx;
	SiftConnProbe *probe;
	AttrNumber	key_att;		/* the scope key in the relation: what the probe reads */
	HTAB	   *seen;
	int64	   *pool;
	uint64		npool;
	uint64		poolcap;
} ScopeMap;

struct SiftScopeFill
{
	SiftColFill hook;			/* what the writers of segments are given (sift_scope_fill_hook) */
	SiftColFill *conn;			/* the fill of the found flags of the same writers, NULL: the index has no connection */
	int			nconn;
	MemoryContext cx;
	Snapshot	snap;			/* kept for the probes until the end call (NULL: none to release) */
	int			n;
	ScopeMap	m[SIFT_SCOPE_MAX];
};

static int
pair_cmp(const void *a, const void *b)
{
	const ScopePair *x = a,
			   *y = b;

	if (x->member != y->member)
		return x->member < y->member ? -1 : 1;
	return x->key < y->key ? -1 : x->key > y->key;
}

static void
count_pair(void *arg, int64 member, int64 key)
{
	(*(uint64 *) arg)++;
}

static void
add_pair(void *arg, int64 member, int64 key)
{
	ScopeMap   *m = arg;

	m->pairs[m->npairs++] = (ScopePair) {member, key};
}

/* Reads the relation into the sorted pairs, unless they need more than limit bytes */
static bool
pairs_load(ScopeMap *m, MemoryContext cx, Relation rel, AttrNumber member, AttrNumber key, Snapshot snap, Size limit)
{
	uint64		n = 0;

	sift_conn_scan_keys(rel, member, key, snap, count_pair, &n);
	if (n > limit / sizeof(ScopePair))
		return false;
	/* check-memory: allow the bytes were checked against the limit of the caller (maintenance_work_mem / 2) above */
	m->pairs = MemoryContextAllocExtended(cx, Max(n, 1) * sizeof(ScopePair), MCXT_ALLOC_HUGE);
	sift_conn_scan_keys(rel, member, key, snap, add_pair, m);
	qsort(m->pairs, m->npairs, sizeof(ScopePair), pair_cmp);
	return true;
}

static HTAB *
seen_new(MemoryContext cx)
{
	HASHCTL		ctl = {.keysize = sizeof(int64),.entrysize = sizeof(ScopeSeen),.hcxt = cx};

	return hash_create("sift scope probe", 1024, &ctl, HASH_ELEM | HASH_BLOBS | HASH_CONTEXT);
}

/*
 * The relation is open (AccessShare, held to the end of the transaction) and btree is its unique index on (member, key), in either order. stamp is the stamp of its journal taken
 * before the relation is read (lineage 0: the journal cannot vouch, the entries are written all the same and a reader will not take them for the relation's).
 */
static void
map_begin(SiftScopeFill *f, ScopeMap *m, Relation rel, Oid btree, AttrNumber member, AttrNumber key, Snapshot snap, uint64 rows, SiftConnStamp stamp)
{
	m->cx = f->cx;
	m->stamp = stamp;
	if (sift_conn_probe_cheaper(rows, sift_conn_rows_of(rel)) || !pairs_load(m, f->cx, rel, member, key, snap, (Size) maintenance_work_mem * 1024 / 2))
	{
		m->probe = sift_conn_probe_begin_on(f->cx, rel, btree, member, snap);
		m->key_att = key;
		m->seen = seen_new(f->cx);
	}
}

static void hook_batch(SiftColFill *hook, SiftColBatch *view);
static SiftConnStamp hook_stamp(const SiftColFill *hook, int i);
static void hook_end(SiftColFill *hook);

static SiftScopeFill *
fill_new(MemoryContext mcxt, int n)
{
	SiftScopeFill *f = MemoryContextAllocZero(mcxt, sizeof(SiftScopeFill));

	f->cx = mcxt;
	f->n = n;
	f->hook = (SiftColFill) {.batch = hook_batch,.stamp = hook_stamp,.end = hook_end};
	/* in parallel mode no snapshot can be taken: the statement's own */
	f->snap = IsInParallelMode() ? GetActiveSnapshot() : RegisterSnapshot(GetTransactionSnapshot());
	return f;
}

/* The maps are made: a fill whose maps all hold their pairs gives the snapshot back, one that probes keeps it until the end call */
static void
fill_done(SiftScopeFill *f)
{
	bool		probing = false;

	for (int i = 0; i < f->n; i++)
		probing |= f->m[i].probe != NULL;
	if (!probing && !IsInParallelMode())
		UnregisterSnapshot(f->snap);
	if (!probing || IsInParallelMode())
		f->snap = NULL;
}

SiftScopeFill *
sift_scope_fill_begin(Relation index, MemoryContext mcxt, uint64 rows)
{
	SiftScopeFill *f;
	SiftScopeSpecs *specs;
	Relation	heap;
	const SiftColRel *r;

	if (!sift_col_is_index(index) || (specs = sift_scope_specs(index)) == NULL)
		return NULL;
	/* a lock taken below can reload the relcache entry of the index and free the state it caches: the fill keeps a copy of its own */
	r = sift_col_rel_private(index, mcxt);
	heap = table_open(index->rd_index->indrelid, AccessShareLock);
	f = fill_new(mcxt, specs->n);
	for (int i = 0; i < f->n; i++)
	{
		Relation	rel;
		AttrNumber	member,
					key;
		Oid			btree = sift_scope_relation_index(heap, index, specs->s[i], &rel, &member, &key);
		SiftConnStamp stamp;

		/* the stamp comes before the first read of the relation */
		if (sift_scope_take(index, i, f->snap, &stamp) != SIFT_CONN_OK)
			stamp.xmin = f->snap->xmin;
		f->m[i].fk_leaf = r->first[r->schema->cols[r->scope_leaf[i]].attno];
		map_begin(f, &f->m[i], rel, btree, member, key, f->snap, rows, stamp);
		table_close(rel, NoLock);
	}
	fill_done(f);
	table_close(heap, NoLock);
	return f;
}

#ifdef SIFT_TESTING
SiftScopeFill *
sift_scope_fill_begin_rel(MemoryContext mcxt, Relation rel, Oid btree, AttrNumber member, AttrNumber key, uint64 rows)
{
	SiftScopeFill *f = fill_new(mcxt, 1);

	map_begin(f, &f->m[0], rel, btree, member, key, f->snap, rows, (SiftConnStamp) {.xmin = f->snap->xmin});
	fill_done(f);
	return f;
}
#endif

void
sift_scope_fill_end(SiftScopeFill *f)
{
	if (f != NULL && f->snap != NULL)
		UnregisterSnapshot(f->snap);
	if (f != NULL)
		f->snap = NULL;
}

SiftConnStamp
sift_scope_fill_stamp(const SiftScopeFill *f, int scope)
{
	return f->m[scope].stamp;
}

int
sift_scope_fill_fk_leaf(const SiftScopeFill *f, int scope)
{
	return f->m[scope].fk_leaf;
}

/* The stamps of a segment of a scope index are those of its connections, then those of its scopes; only the found flags are filled per batch */
static void
hook_batch(SiftColFill *hook, SiftColBatch *view)
{
	SiftScopeFill *f = (SiftScopeFill *) hook;

	f->conn->batch(f->conn, view);
}

static SiftConnStamp
hook_stamp(const SiftColFill *hook, int i)
{
	const SiftScopeFill *f = (const SiftScopeFill *) hook;

	return i < f->nconn ? f->conn->stamp(f->conn, i) : f->m[i - f->nconn].stamp;
}

static void
hook_end(SiftColFill *hook)
{
	SiftScopeFill *f = (SiftScopeFill *) hook;

	sift_conn_fill_end(f->conn);
	sift_scope_fill_end(f);
}

SiftColFill *
sift_scope_fill_hook(SiftScopeFill *f, SiftColFill *conn, int nconn)
{
	f->conn = conn;
	f->nconn = nconn;
	return &f->hook;
}

typedef struct ProbeKeys
{
	ScopeMap   *m;
	ScopeSeen  *e;
} ProbeKeys;

static bool
collect_key(void *arg, TupleTableSlot *slot)
{
	ProbeKeys  *pk = arg;
	ScopeMap   *m = pk->m;
	int64		key;

	if (!sift_conn_slot_key(slot, m->key_att, &key))
		return true;
	if (m->npool == m->poolcap)
	{
		m->poolcap = Max(m->poolcap * 2, 1024);
		m->pool = m->pool == NULL ? MemoryContextAllocExtended(m->cx, m->poolcap * sizeof(int64), MCXT_ALLOC_HUGE)
			: repalloc_huge(m->pool, m->poolcap * sizeof(int64));
	}
	m->pool[m->npool++] = key;
	pk->e->n++;
	return true;
}

/* The keys of every member of the batch are in the cache */
static void
probe_members(ScopeMap *m, const int64 *fk, const uint64 *nulls, uint32 nrows)
{
	SiftConnProbeScan s;

	if (hash_get_num_entries(m->seen) > SEEN_MAX || m->npool > POOL_MAX)
	{
		hash_destroy(m->seen);
		m->seen = seen_new(m->cx);
		m->npool = 0;
	}
	sift_conn_probe_open(m->probe, &s);
	for (uint32 row = 0; row < nrows; row++)
	{
		bool		hit;
		ScopeSeen  *e;
		ProbeKeys	pk;

		if (sift_col_bit_test(nulls, row))
			continue;
		e = hash_search(m->seen, &fk[row], HASH_ENTER, &hit);
		if (hit)
			continue;
		e->start = m->npool;
		e->n = 0;
		pk = (ProbeKeys) {m, e};
		sift_conn_probe_key(m->probe, &s, fk[row], collect_key, &pk);
	}
	sift_conn_probe_close(&s);
}

static void
put(uint32 *n, uint32 **row, int64 **key, uint32 *cap, uint32 r, int64 k)
{
	if (*n == *cap)
	{
		*cap = Max(*cap * 2, 1024);
		*row = *row == NULL ? palloc_extended(*cap * sizeof(uint32), MCXT_ALLOC_HUGE) : repalloc_huge(*row, *cap * sizeof(uint32));
		*key = *key == NULL ? palloc_extended(*cap * sizeof(int64), MCXT_ALLOC_HUGE) : repalloc_huge(*key, *cap * sizeof(int64));
	}
	(*row)[*n] = r;
	(*key)[(*n)++] = k;
}

/* The first pair of the key set that has member or a larger one */
static uint64
pairs_first(const ScopeMap *m, int64 member)
{
	uint64		lo = 0,
				hi = m->npairs;

	while (lo < hi)
	{
		uint64		mid = lo + (hi - lo) / 2;

		if (m->pairs[mid].member < member)
			lo = mid + 1;
		else
			hi = mid;
	}
	return lo;
}

uint32
sift_scope_fill_expand(SiftScopeFill *f, int scope, const int64 *fk, const uint64 *nulls, uint32 nrows, uint32 **row, int64 **key, uint32 *cap)
{
	ScopeMap   *m = &f->m[scope];
	uint32		n = 0;

	if (m->probe != NULL)
		probe_members(m, fk, nulls, nrows);
	for (uint32 r = 0; r < nrows; r++)
	{
		if (sift_col_bit_test(nulls, r))
			continue;
		if (m->probe != NULL)
		{
			const ScopeSeen *e = hash_search(m->seen, &fk[r], HASH_FIND, NULL);

			for (uint32 j = 0; j < e->n; j++)
				put(&n, row, key, cap, r, m->pool[e->start + j]);
		}
		else
			for (uint64 i = pairs_first(m, fk[r]); i < m->npairs && m->pairs[i].member == fk[r]; i++)
				put(&n, row, key, cap, r, m->pairs[i].key);
	}
	return n;
}
