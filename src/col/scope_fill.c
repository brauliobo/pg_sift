/*
 * col/scope_fill.c - the fill of a scope index (col/scope.h): the scope keys of the member keys of a batch of fact rows, under one stamp of the scope relation
 *
 * For each scope the fill knows the pairs (member, scope key) of the relation under a snapshot the way the fill of the found flags knows a parent (col/conn_fill.c): by reading the relation
 * once into the pairs sorted by member, or by asking its btree for the members of each batch, whichever the rows to fill make cheaper (the same price rule and the same btree probe). The stamp is taken before the
 * relation is read, from the journal of the relation (col/scope_journal.c): lineage 0 (and the snapshot's xmin) when the relation has no journal that vouches for the snapshot. A scope
 * of two paths has a stamp for each of its three journals.
 * The relation may hold a pair in several rows (a btree that is not unique): what is read or probed is made distinct by sift_scope_pairs_distinct (col/scan/scope.c), a list of pairs, a probe's answer for a member and the
 * union of the paths alike, so that a fact is never expanded twice by one key.
 * A scope of two paths has the DISTINCT union of the pairs of its relation and of the joined ones, (member, key) for each row of the second relation (via, key) and each member of the third
 * (via, member) with the same via: the three are read, or asked, in the way of one, and a member's keys are the keys of its pairs and of the keys of each of its via values.
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

/* The cut of the units of a scope (cut[], SIFT_COL_CUT_MAX leaves at most): its derived key, then the first date or timestamp column of the index, else none */
int
sift_scope_cut(const SiftColRel *r, int scope, int *cut)
{
	int			time = sift_scope_time_leaf(r->schema->cols, r->schema->ncols),
				n = 0;

	cut[n++] = r->scope_leaf[scope];
	if (time >= 0)
		cut[n++] = time;
	return n;
}

/* The members asked are remembered up to this many (or this many of their keys), then forgotten at the next batch */
#define SEEN_MAX	(1 << 18)
#define POOL_MAX	((uint64) 1 << 22)

/* A probe's answer for a member: its keys are pool[start, start + n), each once */
typedef struct ScopeSeen
{
	int64		member;
	uint32		start;
	uint32		n;
} ScopeSeen;

/* The relation of one path with its btree and its columns: the pairs it gives are (a, b) */
typedef struct ScopeSrc
{
	Relation	rel;
	Oid			btree;
	AttrNumber	a;
	AttrNumber	b;
	SiftKeyMap *km;				/* column b is a text, varchar or uuid column: its values are handed on as the codes of this map, interned (a writer) */
	SiftKeyMap *kma;			/* column a is: its values are handed on as the codes of this map, which has no store (the codes of the statement) */
} ScopeSrc;

typedef struct ScopeList
{
	SiftScopePair *p;
	uint64		n;
} ScopeList;

/* The btree asked for the values of its first column, the answers remembered: the b column of the rows of each value */
typedef struct ScopeProbe
{
	MemoryContext cx;
	SiftConnProbe *probe;
	AttrNumber	out;			/* the column the probe reads of each row */
	SiftKeyMap *km;				/* ... which is mapped to codes (NULL: an integer column) */
	HTAB	   *seen;
	SiftScopePair *pool;			/* (the probed value, one key of its rows) */
	uint64		npool;
	uint64		poolcap;
} ScopeProbe;

typedef struct ScopeMap
{
	int			njournals;		/* the stamps of the scope: 1, or one for each journal of two paths */
	SiftConnStamp stamp[SIFT_SCOPE_JOURNALS];
	int			fk_leaf;		/* the leaf of the fact index that holds the member key */
	SiftKeyMap *kma;			/* the member of the pairs is a text, varchar or uuid value: the code the map of the fill gives it is its member key */
	int64	   *ids;			/* the member keys of the batch in hand (sift_scope_fill_members) */
	uint32		idcap;
	SiftScopePair *pairs;			/* the key set: every pair, sorted and distinct (NULL: the probes) */
	uint64		npairs;
	MemoryContext cx;
	bool		hops;			/* two paths: the keys of a member are those of d and those of g of each value of v */
	ScopeProbe	d;				/* member -> keys of the pairs relation */
	ScopeProbe	v;				/* two paths: member -> via values (the members of the via relation, asked by member) */
	ScopeProbe	g;				/* two paths: via value -> keys of the joined relation */
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

static void
count_pair(void *arg, int64 a, int64 b)
{
	(*(uint64 *) arg)++;
}

static void
add_pair(void *arg, int64 a, int64 b)
{
	ScopeList  *l = arg;

	l->p[l->n++] = (SiftScopePair) {a, b};
}

/* Reads the pairs (a, b) of the source into the list, sorted and distinct (a row of the relation each, a pair may be several), unless they need more than *budget bytes; else takes what they need from it */
static bool
list_load(ScopeList *l, MemoryContext cx, const ScopeSrc *src, Snapshot snap, Size *budget)
{
	uint64		n = 0;

	/* the counting scan meets every key of the relation: a map has them interned together, one run, before the scan that lists the pairs reads their codes */
	if (src->km != NULL)
		sift_keymap_collect(src->km, true);
	sift_conn_scan_pairs(src->rel, src->btree, src->a, src->kma, src->b, src->km, snap, count_pair, &n);
	if (src->km != NULL)
		sift_keymap_collect(src->km, false);
	if (n > *budget / sizeof(SiftScopePair))
		return false;
	*budget -= n * sizeof(SiftScopePair);
	/* check-memory: allow the bytes were checked against the budget of the caller (maintenance_work_mem / 2) above */
	l->p = MemoryContextAllocExtended(cx, Max(n, 1) * sizeof(SiftScopePair), MCXT_ALLOC_HUGE);
	l->n = 0;
	sift_conn_scan_pairs(src->rel, src->btree, src->a, src->kma, src->b, src->km, snap, add_pair, l);
	l->n = sift_scope_pairs_distinct(l->p, l->n);
	return true;
}

/* The pairs (member, key) of the rows of the joined relation g (via, key) and the members of the via relation v (via, member) with the same via, in the array out of at most max, from *n on; returns how many there are */
static uint64
join_via(const ScopeList *v, const ScopeList *g, SiftScopePair *out, uint64 *n)
{
	uint64		total = 0;
	uint64		i = 0,
				j = 0;

	while (i < v->n && j < g->n)
	{
		uint64		ie = i,
					je = j;

		if (v->p[i].member != g->p[j].member)
		{
			if (v->p[i].member < g->p[j].member)
				i++;
			else
				j++;
			continue;
		}
		while (ie < v->n && v->p[ie].member == v->p[i].member)
			ie++;
		while (je < g->n && g->p[je].member == g->p[j].member)
			je++;
		total += (ie - i) * (je - j);
		if (out != NULL)
			for (uint64 x = i; x < ie; x++)
				for (uint64 y = j; y < je; y++)
					out[(*n)++] = (SiftScopePair) {v->p[x].key, g->p[y].key};
		i = ie;
		j = je;
	}
	return total;
}

/*
 * Reads the pairs of the scope into the sorted, distinct key set, unless they need more than limit bytes. The pairs of two paths are the direct ones and the join of the other two relations
 * on the via column (all three are read: the join is made of their sorted lists, a member of a via value with each key of it).
 */
static bool
pairs_load(ScopeMap *m, MemoryContext cx, const ScopeSrc *src, int nsrc, Snapshot snap, Size limit)
{
	ScopeList	d = {0},
				v = {0},
				g = {0};
	Size		budget = limit;
	uint64		nj;
	bool		ok = list_load(&d, cx, &src[0], snap, &budget);

	if (ok && nsrc == 1)
	{
		m->pairs = d.p;
		m->npairs = d.n;
		return true;
	}
	ok = ok && list_load(&v, cx, &src[2], snap, &budget) && list_load(&g, cx, &src[1], snap, &budget);
	nj = ok ? join_via(&v, &g, NULL, NULL) : 0;
	ok = ok && nj <= budget / sizeof(SiftScopePair) - d.n;
	if (ok)
	{
		/* check-memory: allow the bytes were checked against the limit of the caller (maintenance_work_mem / 2) above */
		m->pairs = MemoryContextAllocExtended(cx, Max(d.n + nj, 1) * sizeof(SiftScopePair), MCXT_ALLOC_HUGE);
		memcpy(m->pairs, d.p, d.n * sizeof(SiftScopePair));
		m->npairs = d.n;
		join_via(&v, &g, m->pairs, &m->npairs);
		m->npairs = sift_scope_pairs_distinct(m->pairs, m->npairs);
	}
	for (ScopeList *l = &d; l != NULL; l = l == &d ? &v : l == &v ? &g : NULL)
		if (l->p != NULL)
			pfree(l->p);
	return ok;
}

static HTAB *
seen_new(MemoryContext cx)
{
	HASHCTL		ctl = {.keysize = sizeof(int64),.entrysize = sizeof(ScopeSeen),.hcxt = cx};

	return hash_create("sift scope probe", 1024, &ctl, HASH_ELEM | HASH_BLOBS | HASH_CONTEXT);
}

/* The btree of the source asked by its column by, the answers the column out of each row */
static void
probe_begin(ScopeProbe *p, MemoryContext cx, const ScopeSrc *src, AttrNumber by, AttrNumber out, Snapshot snap)
{
	p->cx = cx;
	p->probe = sift_conn_probe_begin_on(cx, src->rel, src->btree, by, snap);
	p->out = out;
	p->km = out == src->b ? src->km : out == src->a ? src->kma : NULL;
	if (by == src->b && src->km != NULL)
		sift_conn_probe_codec(p->probe, src->km);
	if (by == src->a && src->kma != NULL)
		sift_conn_probe_codec(p->probe, src->kma);
	p->seen = seen_new(cx);
}

/*
 * The sources are open (AccessShare, held to the end of the transaction), each with its btree on its two columns, in either order: the pairs relation, and for two paths the joined
 * relation (via, key) and the via relation (via, member). stamp is the stamp of each journal taken before the relations are read (lineage 0: the journal cannot vouch, the
 * entries are written all the same and a reader will not take them for the relations').
 */
static void
map_begin(SiftScopeFill *f, ScopeMap *m, const ScopeSrc *src, int nsrc, Snapshot snap, uint64 rows, const SiftConnStamp *stamp)
{
	double		parent_rows = 0;

	m->cx = f->cx;
	m->njournals = nsrc == 3 ? SIFT_SCOPE_JOURNALS : 1;
	memcpy(m->stamp, stamp, m->njournals * sizeof(SiftConnStamp));
	m->hops = nsrc == 3;
	m->kma = src[0].kma;
	for (int i = 0; i < nsrc; i++)
		parent_rows += sift_conn_rows_of(src[i].rel);
	if (sift_conn_probe_cheaper(rows, parent_rows) || !pairs_load(m, f->cx, src, nsrc, snap, (Size) maintenance_work_mem * 1024 / 2))
	{
		probe_begin(&m->d, f->cx, &src[0], src[0].a, src[0].b, snap);
		if (m->hops)
		{
			probe_begin(&m->v, f->cx, &src[2], src[2].b, src[2].a, snap);
			probe_begin(&m->g, f->cx, &src[1], src[1].a, src[1].b, snap);
		}
	}
}

static void hook_batch(SiftColFill *hook, SiftColBatch *view);
static SiftConnStamp hook_stamp(const SiftColFill *hook, int i);
static int	hook_scope_journals(const SiftColFill *hook, int scope);
static SiftConnStamp hook_scope_stamp(const SiftColFill *hook, int scope, int journal);
static void hook_end(SiftColFill *hook);

static SiftScopeFill *
fill_new(MemoryContext mcxt, int n)
{
	SiftScopeFill *f = MemoryContextAllocZero(mcxt, sizeof(SiftScopeFill));

	f->cx = mcxt;
	f->n = n;
	f->hook = (SiftColFill) {.batch = hook_batch,.stamp = hook_stamp,.scope_journals = hook_scope_journals,.scope_stamp = hook_scope_stamp,.end = hook_end};
	return f;
}

/* in parallel mode no snapshot can be taken: the statement's own */
static void
fill_snapshot(SiftScopeFill *f)
{
	f->snap = IsInParallelMode() ? GetActiveSnapshot() : RegisterSnapshot(GetLatestSnapshot());
}

/* The maps are made: a fill whose maps all hold their pairs gives the snapshot back, one that probes keeps it until the end call */
static void
fill_done(SiftScopeFill *f)
{
	bool		probing = false;

	for (int i = 0; i < f->n; i++)
		probing |= f->m[i].d.probe != NULL;
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
	fill_snapshot(f);
	for (int i = 0; i < f->n; i++)
	{
		const SiftScopeSpec *sp = specs->s[i];
		int			nsrc = sp->joined ? 3 : 1;
		ScopeSrc	src[3];
		SiftConnStamp stamp[SIFT_SCOPE_JOURNALS];

		src[0].btree = sift_scope_relation_index(heap, index, sp, &src[0].rel, &src[0].a, &src[0].b);
		{
			MemoryContext old = MemoryContextSwitchTo(mcxt);

			src[0].km = sift_keymap_open(src[0].rel, src[0].b, true);
			src[0].kma = sift_keymap_transient(src[0].rel, src[0].a);
			MemoryContextSwitchTo(old);
		}
		for (int h = 1; h < nsrc; h++)
		{
			src[h].btree = sift_scope_hop_index(heap, index, sp, h, &src[h].rel, &src[h].a, &src[h].b);
			src[h].km = src[h].kma = NULL;
		}
		/* the stamps come before the first read of the relations */
		if (sift_scope_take(index, i, f->snap, stamp) != SIFT_CONN_OK)
			for (int j = 0; j < nsrc; j++)
				stamp[j].xmin = f->snap->xmin;
		f->m[i].fk_leaf = r->first[r->schema->cols[r->scope_leaf[i]].attno];
		map_begin(f, &f->m[i], src, nsrc, f->snap, rows, stamp);
		for (int h = 0; h < nsrc; h++)
			table_close(src[h].rel, NoLock);
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
	ScopeSrc	src = {rel, btree, member, key, NULL, NULL};

	fill_snapshot(f);
	map_begin(f, &f->m[0], &src, 1, f->snap, rows, &(SiftConnStamp) {.xmin = f->snap->xmin});
	fill_done(f);
	return f;
}
#endif

/* A fill that expands nothing and stamps the scopes of the index as it is told: what the merge of segments writes, whose entries are copies of the inputs' */
SiftScopeFill *
sift_scope_fill_carry(Relation index, MemoryContext mcxt, const SiftConnStamp (*stamp)[SIFT_SCOPE_JOURNALS])
{
	SiftScopeSpecs *specs = sift_scope_specs(index);
	SiftScopeFill *f = fill_new(mcxt, specs->n);

	for (int i = 0; i < f->n; i++)
	{
		f->m[i].njournals = sift_scope_njournals(specs->s[i]);
		memcpy(f->m[i].stamp, stamp[i], f->m[i].njournals * sizeof(SiftConnStamp));
	}
	return f;
}

/*
 * The stamp of a merge of segments stamped a and b: the older one, which every member the relation changed since either is in the changes of (col/conn.h); the stamps of
 * two lineages, and a stamp that was not taken (no xmin), leave none (all zero: the segment says nothing).
 */
SiftConnStamp
sift_scope_stamp_older(SiftConnStamp a, SiftConnStamp b)
{
	if (a.lineage != b.lineage || !TransactionIdIsNormal(a.xmin) || !TransactionIdIsNormal(b.xmin))
		return (SiftConnStamp) {0};
	return TransactionIdPrecedes(a.xmin, b.xmin) ? a : b;
}

void
sift_scope_fill_end(SiftScopeFill *f)
{
	if (f != NULL && f->snap != NULL)
		UnregisterSnapshot(f->snap);
	if (f != NULL)
		f->snap = NULL;
}

int
sift_scope_fill_fk_leaf(const SiftScopeFill *f, int scope)
{
	return f->m[scope].fk_leaf;
}

/* The stamps of a segment of a scope index are those of its connections, then those of the journals of each scope; only the found flags are filled per batch */
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

	return f->conn->stamp(f->conn, i);
}

static int
hook_scope_journals(const SiftColFill *hook, int scope)
{
	return ((const SiftScopeFill *) hook)->m[scope].njournals;
}

static SiftConnStamp
hook_scope_stamp(const SiftColFill *hook, int scope, int journal)
{
	return ((const SiftScopeFill *) hook)->m[scope].stamp[journal];
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
	ScopeProbe *p;
	ScopeSeen  *e;
	int64		val;
} ProbeKeys;

static bool
collect_key(void *arg, TupleTableSlot *slot)
{
	ProbeKeys  *pk = arg;
	ScopeProbe *p = pk->p;
	int64		key;

	if (!sift_conn_slot_code(slot, p->out, p->km, &key))
		return true;
	if (p->npool == p->poolcap)
	{
		p->poolcap = Max(p->poolcap * 2, 1024);
		p->pool = p->pool == NULL ? MemoryContextAllocExtended(p->cx, p->poolcap * sizeof(SiftScopePair), MCXT_ALLOC_HUGE)
			: repalloc_huge(p->pool, p->poolcap * sizeof(SiftScopePair));
	}
	p->pool[p->npool++] = (SiftScopePair) {pk->val, key};
	pk->e->n++;
	return true;
}

/* The answers for every value of the batch (nulls: bit set = NULL, NULL: none) are in the cache */
static void
probe_values(ScopeProbe *p, const int64 *vals, const uint64 *nulls, uint32 n)
{
	SiftConnProbeScan s;

	if (hash_get_num_entries(p->seen) > SEEN_MAX || p->npool > POOL_MAX)
	{
		hash_destroy(p->seen);
		p->seen = seen_new(p->cx);
		p->npool = 0;
	}
	sift_conn_probe_open(p->probe, &s);
	if (p->km != NULL)
		sift_keymap_hold(p->km);
	for (uint32 i = 0; i < n; i++)
	{
		bool		hit;
		ScopeSeen  *e;
		ProbeKeys	pk;

		if (nulls != NULL && sift_col_bit_test(nulls, i))
			continue;
		e = hash_search(p->seen, &vals[i], HASH_ENTER, &hit);
		if (hit)
			continue;
		e->start = p->npool;
		e->n = 0;
		pk = (ProbeKeys) {p, e, vals[i]};
		sift_conn_probe_key(p->probe, &s, vals[i], collect_key, &pk);
		e->n = sift_scope_pairs_distinct(p->pool + e->start, e->n);
		p->npool = e->start + e->n;
	}
	if (p->km != NULL)
		sift_keymap_release(p->km);
	sift_conn_probe_close(&s);
}

/* The answer of the cache for a value it holds */
static const ScopeSeen *
seen_of(const ScopeProbe *p, int64 val)
{
	return hash_search(p->seen, &val, HASH_FIND, NULL);
}

/* Appends k values to the array a of *n, grown as it needs (*cap) */
static void
append_i64(int64 **a, uint64 *cap, uint64 *n, const int64 *src, uint64 k)
{
	if (*n + k > *cap)
	{
		*cap = Max(*cap * 2, *n + k);
		/* check-memory: allow the values of one batch or one member, as many as the answers of the caches hold for them */
		*a = *a == NULL ? palloc_extended(*cap * sizeof(int64), MCXT_ALLOC_HUGE) : repalloc_huge(*a, *cap * sizeof(int64));
	}
	memcpy(*a + *n, src, k * sizeof(int64));
	*n += k;
}

/* The via values of the members of the batch are in the cache of the joined relation */
static void
probe_groups(ScopeMap *m, const int64 *fk, const uint64 *nulls, uint32 nrows)
{
	int64	   *vias = NULL;
	uint64		n = 0,
				cap = 0;

	for (uint32 r = 0; r < nrows; r++)
		if (!sift_col_bit_test(nulls, r))
		{
			const ScopeSeen *e = seen_of(&m->v, fk[r]);

			for (uint32 j = 0; j < e->n; j++)
				append_i64(&vias, &cap, &n, &m->v.pool[e->start + j].key, 1);
		}
	if (n > 0)
		probe_values(&m->g, vias, NULL, (uint32) n);
	if (vias != NULL)
		pfree(vias);
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

/* Appends the keys of the pairs of src (the answers of a probe) as pairs of member to the array of *n, grown as it needs (*cap) */
static void
append_pairs(SiftScopePair **a, uint64 *cap, uint64 *n, int64 member, const SiftScopePair *src, uint64 k)
{
	if (*n + k > *cap)
	{
		*cap = Max(*cap * 2, *n + k);
		/* check-memory: allow the keys of one member: its direct pairs and those of each of its via values */
		*a = *a == NULL ? palloc_extended(*cap * sizeof(SiftScopePair), MCXT_ALLOC_HUGE) : repalloc_huge(*a, *cap * sizeof(SiftScopePair));
	}
	for (uint64 i = 0; i < k; i++)
		(*a)[(*n)++] = (SiftScopePair) {member, src[i].key};
}

/* The pairs of a member of a scope of two paths: those of its direct rows and of each of its via values, each once, ascending by key, in *pairs (grown as it needs) */
static uint64
member_pairs(const ScopeMap *m, int64 member, SiftScopePair **pairs, uint64 *cap)
{
	const ScopeSeen *d = seen_of(&m->d, member),
			   *v = seen_of(&m->v, member);
	uint64		n = 0;

	append_pairs(pairs, cap, &n, member, m->d.pool + d->start, d->n);
	for (uint32 j = 0; j < v->n; j++)
	{
		const ScopeSeen *e = seen_of(&m->g, m->v.pool[v->start + j].key);

		append_pairs(pairs, cap, &n, member, m->g.pool + e->start, e->n);
	}
	return sift_scope_pairs_distinct(*pairs, n);
}

/* The member key of a value of the member column (of type) of the fact table: its integer, or the code the map of the scope gives it */
int64
sift_scope_fill_member(SiftScopeFill *f, int scope, Oid type, Datum d)
{
	int64		k;

	if (f->m[scope].kma != NULL)
		(void) sift_keymap_code(f->m[scope].kma, d, &k);
	else
		k = type == INT2OID ? DatumGetInt16(d) : type == INT4OID ? DatumGetInt32(d) : DatumGetInt64(d);
	return k;
}

/*
 * The member keys of the rows of a batch, for sift_scope_fill_expand: the integers of its member leaf, or for a text, varchar or uuid member the code the map of the scope gives its value (one
 * lookup for each value of the dictionary of the batch that a row holds, not for each row). A NULL row has none.
 */
const int64 *
sift_scope_fill_members(SiftScopeFill *f, int scope, const SiftColBatch *b)
{
	ScopeMap   *m = &f->m[scope];
	const SiftCVals *fk = &b->col[m->fk_leaf];

	if (m->kma == NULL)
		return fk->key;
	if (m->idcap < b->nrows)
	{
		/* check-memory: allow-allocs the rows of one batch, which the fill bounds */
		m->ids = m->ids == NULL ? MemoryContextAlloc(f->cx, b->nrows * sizeof(int64)) : repalloc(m->ids, b->nrows * sizeof(int64));
		m->idcap = b->nrows;
	}
	sift_keymap_batch_codes(m->kma, b->dict[m->fk_leaf], fk->key, fk->nulls, b->nrows, true, m->ids);
	return m->ids;
}

uint32
sift_scope_fill_expand(SiftScopeFill *f, int scope, const int64 *fk, const uint64 *nulls, uint32 nrows, uint32 **row, int64 **key, uint32 *cap)
{
	ScopeMap   *m = &f->m[scope];
	SiftScopePair *own = NULL;
	uint64		owncap = 0;
	uint32		n = 0;

	if (m->d.probe != NULL)
		probe_values(&m->d, fk, nulls, nrows);
	if (m->d.probe != NULL && m->hops)
	{
		probe_values(&m->v, fk, nulls, nrows);
		probe_groups(m, fk, nulls, nrows);
	}
	for (uint32 r = 0; r < nrows; r++)
	{
		if (sift_col_bit_test(nulls, r))
			continue;
		if (m->d.probe != NULL && m->hops)
		{
			uint64		nk = member_pairs(m, fk[r], &own, &owncap);

			for (uint64 j = 0; j < nk; j++)
				put(&n, row, key, cap, r, own[j].key);
		}
		else if (m->d.probe != NULL)
		{
			const ScopeSeen *e = seen_of(&m->d, fk[r]);

			for (uint32 j = 0; j < e->n; j++)
				put(&n, row, key, cap, r, m->d.pool[e->start + j].key);
		}
		else
			for (uint64 i = pairs_first(m, fk[r]); i < m->npairs && m->pairs[i].member == fk[r]; i++)
				put(&n, row, key, cap, r, m->pairs[i].key);
	}
	if (own != NULL)
		pfree(own);
	return n;
}
