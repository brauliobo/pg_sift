/*
 * agg/v6/dim_cache.c - the dimensions of a star join kept for the next statement (agg/v6/dim_cache.h).
 */
#include "postgres.h"

#include "access/genam.h"
#include "miscadmin.h"
#include "nodes/nodeFuncs.h"
#include "optimizer/optimizer.h"
#include "storage/relfilelocator.h"
#include "utils/inval.h"
#include "utils/memutils.h"
#include "utils/rel.h"
#include "utils/relcache.h"
#include "utils/rls.h"
#include "utils/syscache.h"

#include "agg/v6/dim_cache.h"
#include "col/am.h"
#include "col/am/conn_route.h"
#include "core/cachebudget.h"

/* what a scan reads of a table: the table, the index it reads by (none: the heap), and the filters it applies to the rows it finds */
typedef struct DimScan
{
	Relation	heap;
	Oid			indexid;
	List	   *filters[2];
} DimScan;

typedef struct DimEnt
{
	char	   *key;
	uint32		rel_hash;		/* the syscache hash values of the pg_class rows of the table and of the index whose journal vouches for it */
	uint32		index_hash;
	RelFileLocator loc;
	SiftConnStamp stamp;
	SiftStarDim *dim;
	MemoryContext cx;			/* the table: the arrays of dim, this entry */
	uint64		used;
	int			pins;
	bool		dead;			/* its table or index changed under a statement that holds it: no statement finds it, the last to let go drops it */
} DimEnt;

struct SiftDimTry
{
	MemoryContext mcx;			/* the statement's: the pins are released when it is reset */
	MemoryContext rcx;
	Snapshot	snap;
	Relation	heap;
	Oid			index;
	SiftConnStamp stamp;
	char	   *key;
};

typedef struct DimPin
{
	MemoryContextCallback cb;
	DimEnt	   *ent;
} DimPin;

static MemoryContext cache_cx;
static List *entries;

static void
ent_drop(DimEnt *e)
{
	entries = list_delete_ptr(entries, e);
	MemoryContextDelete(e->cx);
}

typedef bool (*EntMatch) (const DimEnt *e, const void *arg);

/* the entries that match are no more found: those nobody holds go, the others when the last statement lets go */
static void
ent_retire(EntMatch match, const void *arg)
{
	for (bool again = true; again;)
	{
		again = false;
		foreach_ptr(DimEnt, e, entries)
		{
			if (e->dead || !match(e, arg))
				continue;
			e->dead = true;
			if (e->pins == 0)
			{
				ent_drop(e);
				again = true;
				break;
			}
		}
	}
}

static bool
match_key(const DimEnt *e, const void *arg)
{
	return strcmp(e->key, (const char *) arg) == 0;
}

static bool
match_rel(const DimEnt *e, const void *arg)
{
	const uint32 hash = *(const uint32 *) arg;

	return hash == 0 || e->rel_hash == hash || e->index_hash == hash;
}

/* a change of the pg_class row of a table or of its index (DROP, TRUNCATE, ALTER, REINDEX, a rewrite), or of every one (hash 0) */
static void
dimcache_inval(Datum arg, int cacheid, uint32 hash)
{
	ent_retire(match_rel, &hash);
}

static DimEnt *
ent_unpinned_oldest(void)
{
	DimEnt	   *lru = NULL;

	foreach_ptr(DimEnt, e, entries)
		if (e->pins == 0 && (lru == NULL || e->used < lru->used))
			lru = e;
	return lru;
}

static uint64
dimcache_oldest(void)
{
	DimEnt	   *e = ent_unpinned_oldest();

	return e != NULL ? e->used : UINT64_MAX;
}

static bool
dimcache_evict_one(void)
{
	DimEnt	   *e = ent_unpinned_oldest();

	if (e == NULL)
		return false;
	ent_drop(e);
	return true;
}

static void
dimcache_evict_all(void)
{
	while (dimcache_evict_one())
		;
}

static const SiftCacheKind dimcache_kind = {"agg_v6_dimensions", dimcache_oldest, dimcache_evict_one, dimcache_evict_all};

static void
pin_release(void *arg)
{
	DimPin	   *p = arg;

	if (p->ent == NULL)
		return;
	if (--p->ent->pins == 0 && p->ent->dead)
		ent_drop(p->ent);
	p->ent = NULL;
}

static DimPin *
ent_pin(DimEnt *e, MemoryContext mcx)
{
	DimPin	   *p = MemoryContextAlloc(mcx, sizeof(DimPin));

	e->pins++;
	e->used = sift_cache_tick();
	p->cb = (MemoryContextCallback) {pin_release, p, NULL};
	p->ent = e;
	MemoryContextRegisterResetCallback(mcx, &p->cb);
	return p;
}

/* the table of a plain scan: no child plan, no parameter of the statement, no sub-select */
static bool
plain_scan(PlanState *ps, DimScan *s)
{
	const Plan *p = ps->plan;

	if (ps->initPlan != NIL || ps->subPlan != NIL)
		return false;
	switch (nodeTag(p))
	{
		case T_SeqScan:
			break;
		case T_IndexScan:
			s->indexid = ((const IndexScan *) p)->indexid;
			s->filters[1] = ((const IndexScan *) p)->indexqualorig;
			break;
		case T_IndexOnlyScan:
			s->indexid = ((const IndexOnlyScan *) p)->indexid;
			s->filters[1] = ((const IndexOnlyScan *) p)->recheckqual;
			break;
		default:
			return false;
	}
	s->filters[0] = p->qual;
	s->heap = ((ScanState *) ps)->ss_currentRelation;
	return true;
}

static bool
unstable_walker(Node *n, void *ctx)
{
	if (n == NULL)
		return false;
	if (IsA(n, Param) || IsA(n, SubPlan) || IsA(n, AlternativeSubPlan))
		return true;
	return expression_tree_walker(n, unstable_walker, ctx);
}

/* a filter that two statements may not read the same: a parameter, a sub-select, or a function that is not immutable */
static bool
unstable(List *filters)
{
	return unstable_walker((Node *) filters, NULL) || contain_mutable_functions((Node *) filters);
}

/* the index whose journal vouches for every change of the table (a partial index does not see the rows outside its predicate), locked; InvalidOid when there is none */
static Oid
journal_index(Relation heap)
{
	List	   *oids = RelationGetIndexList(heap);
	Oid			found = InvalidOid;

	foreach_oid(ioid, oids)
	{
		Relation	idx = index_open(ioid, AccessShareLock);

		if (!OidIsValid(found) && sift_index_ready(idx) && idx->rd_index->indisvalid && sift_col_is_index(idx) && RelationGetIndexPredicate(idx) == NIL && sift_jrnl_on(idx))
			found = ioid;
		index_close(idx, found == ioid ? NoLock : AccessShareLock);
	}
	list_free(oids);
	return found;
}

/* the rows a table stands for: the table and the user, the plan's filters and index, and the columns it is read into (what a statement may find for it) */
static char *
identity(const PlanState *ps, const DimScan *s, const SiftV6XDim *xd, const SiftStarShape *sh)
{
	StringInfoData id;
	const List *tlist = ps->plan->targetlist;

	initStringInfo(&id);
	appendStringInfo(&id, "%u:%u:%u:%d:%d:%d", RelationGetRelid(s->heap), GetUserId(), s->indexid, sh->mode, sh->nkeys, sh->npay);
	for (int k = 0; k < sh->nkeys; k++)
		appendStringInfo(&id, "|%d:%s", sh->kcls[k], nodeToString(((TargetEntry *) list_nth(tlist, xd->kcol[k] - 1))->expr));
	for (int c = 0; c < sh->npay; c++)
		appendStringInfo(&id, "|%d:%s", sh->pcls[c], nodeToString(((TargetEntry *) list_nth(tlist, xd->pcol[c] - 1))->expr));
	appendStringInfo(&id, "|%s|%s", nodeToString(s->filters[0]), nodeToString(s->filters[1]));
	return id.data;
}

/* may a statement under the snapshot read what the stamp stands for: the journal vouches for the snapshot, no row was inserted and no key changed since (the count of the inserts
 * writes the records of this transaction held back, so it comes before the keys) */
static bool
readable(const SiftDimTry *t, const SiftConnStamp *stamp)
{
	Relation	index = index_open(t->index, NoLock);
	SiftConnKeys keys = {0};
	bool		ok = sift_jrnl_vouches(t->heap, index, stamp, t->snap) == SIFT_CONN_OK && sift_jrnl_inserted_rows(t->heap, index, stamp) == 0 &&
		sift_jrnl_changed(t->heap, index, stamp, t->snap, 0, &keys) == SIFT_CONN_OK;

	index_close(index, NoLock);
	return ok;
}

SiftDimTry *
sift_dimcache_try(PlanState *ps, const SiftV6XDim *xd, const SiftStarShape *sh, MemoryContext mcx)
{
	DimScan		s = {0};
	SiftDimTry *t;
	Relation	index;
	Oid			ioid;
	bool		stamped;

	if (!plain_scan(ps, &s) || unstable(s.filters[0]) || unstable(s.filters[1]) || check_enable_rls(RelationGetRelid(s.heap), InvalidOid, true) != RLS_NONE)
		return NULL;
	if (!OidIsValid(ioid = journal_index(s.heap)))
		return NULL;
	t = MemoryContextAllocZero(mcx, sizeof(SiftDimTry));
	t->mcx = mcx;
	t->snap = ps->state->es_snapshot;
	t->heap = s.heap;
	t->index = ioid;
	index = index_open(ioid, NoLock);
	stamped = sift_jrnl_take(s.heap, index, t->snap, &t->stamp) == SIFT_CONN_OK;
	index_close(index, NoLock);
	if (!stamped)
	{
		pfree(t);
		return NULL;
	}
	t->key = MemoryContextStrdup(mcx, identity(ps, &s, xd, sh));
	return t;
}

bool
sift_dimcache_find(SiftDimTry *t, uint64 budget, SiftStarDim *out)
{
	DimEnt	   *found = NULL;
	DimPin	   *pin;

	foreach_ptr(DimEnt, e, entries)
		if (!e->dead && strcmp(e->key, t->key) == 0)
			found = e;
	if (found == NULL || found->dim->bytes > budget || !RelFileLocatorEquals(found->loc, t->heap->rd_locator))
		return false;
	pin = ent_pin(found, t->mcx);
	if (!readable(t, &found->stamp) || found->dead)
	{
		pin_release(pin);
		return false;
	}
	*out = *found->dim;
	return true;
}

MemoryContext
sift_dimcache_read_cx(SiftDimTry *t)
{
	t->rcx = sift_cache_create_context_in(t->mcx, "sift cache v6 dimension");
	return t->rcx;
}

bool
sift_dimcache_keep(SiftDimTry *t, const SiftStarDim *dim)
{
	DimEnt	   *e;
	MemoryContext old;

	if (!readable(t, &t->stamp))
		return false;
	e = MemoryContextAllocZero(t->rcx, sizeof(DimEnt));
	e->key = MemoryContextStrdup(t->rcx, t->key);
	e->dim = MemoryContextAlloc(t->rcx, sizeof(SiftStarDim));
	*e->dim = *dim;
	if (!sift_cache_room(MemoryContextMemAllocated(t->rcx, true)))
		return false;
	if (cache_cx == NULL)
	{
		cache_cx = sift_cache_create_context("sift cache v6 dimensions");
		sift_cache_register(&dimcache_kind);
		CacheRegisterSyscacheCallback(RELOID, dimcache_inval, (Datum) 0);
	}
	e->rel_hash = GetSysCacheHashValue1(RELOID, ObjectIdGetDatum(RelationGetRelid(t->heap)));
	e->index_hash = GetSysCacheHashValue1(RELOID, ObjectIdGetDatum(t->index));
	e->loc = t->heap->rd_locator;
	e->stamp = t->stamp;
	e->cx = t->rcx;
	ent_retire(match_key, t->key);
	old = MemoryContextSwitchTo(cache_cx);
	entries = lappend(entries, e);
	MemoryContextSwitchTo(old);
	MemoryContextSetParent(t->rcx, cache_cx);
	ent_pin(e, t->mcx);
	sift_cache_commit();
	return true;
}
