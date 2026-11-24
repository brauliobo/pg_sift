/*
 * bm25/semi_cache.c - the key sets of the small tables a ranked scan tests an INCLUDE column against (SIFT_PRIV_SEMI), kept by the backend for the next statement.
 *
 * What a snapshot sees of a table is told by one number when it lists no running transaction: a row is visible when its inserting transaction is below the snapshot's
 * xmax and committed, so two such snapshots of one xmax see the same rows, and a statement that changes the table (an insert, a delete, a rewrite, a TRUNCATE all take a transaction
 * number) moves the next xmax up. A backend that has taken a number itself (its own rows are visible by command), a SERIALIZABLE one (which must lock what it reads), a snapshot of
 * a standby, and one that lists running transactions keep and use nothing. No relcache callback is taken (scripts/check-relcache-slots.sh): the xmax is the whole of the test.
 */
#include "postgres.h"

#include "access/transam.h"
#include "access/xact.h"
#include "utils/memutils.h"

#include "bm25/semi_cache.h"
#include "core/cachebudget.h"

#define SEMI_ENTRIES	16

typedef struct SemiEnt
{
	MemoryContext cx;			/* NULL: the slot is free */
	Oid			dim;
	AttrNumber	attno;
	char	   *quals;
	uint64		xmax;
	int			nset;
	int64	   *set;
	uint64		used;			/* sift_cache_tick() of the last use */
} SemiEnt;

static SemiEnt ents[SEMI_ENTRIES];
static bool registered;

static bool
settled_xmax(Snapshot snap, uint64 *xmax)
{
	if (snap->snapshot_type != SNAPSHOT_MVCC || snap->xcnt != 0 || snap->subxcnt != 0 || snap->suboverflowed || snap->takenDuringRecovery || IsolationIsSerializable() ||
		TransactionIdIsValid(GetTopTransactionIdIfAny()) || TransactionIdIsValid(GetCurrentTransactionIdIfAny()))
		return false;
	*xmax = U64FromFullTransactionId(FullTransactionIdFromAllowableAt(ReadNextFullTransactionId(), snap->xmax));
	return true;
}

static void
ent_drop(SemiEnt *e)
{
	MemoryContextDelete(e->cx);
	e->cx = NULL;
}

static SemiEnt *
ent_oldest_used(void)
{
	SemiEnt    *oldest = NULL;

	for (int i = 0; i < SEMI_ENTRIES; i++)
		if (ents[i].cx != NULL && (oldest == NULL || ents[i].used < oldest->used))
			oldest = &ents[i];
	return oldest;
}

static uint64
semi_oldest(void)
{
	SemiEnt    *e = ent_oldest_used();

	return e ? e->used : UINT64_MAX;
}

static bool
semi_evict_one(void)
{
	SemiEnt    *e = ent_oldest_used();

	if (e == NULL)
		return false;
	ent_drop(e);
	return true;
}

static void
semi_evict_all(void)
{
	while (semi_evict_one())
		;
}

static const SiftCacheKind semi_kind = {"bm25_semi_sets", semi_oldest, semi_evict_one, semi_evict_all};

static SemiEnt *
ent_free(void)
{
	for (int i = 0; i < SEMI_ENTRIES; i++)
		if (ents[i].cx == NULL)
			return &ents[i];
	return NULL;
}

static SemiEnt *
ent_find(Oid dim, AttrNumber attno, const char *quals, uint64 xmax)
{
	for (int i = 0; i < SEMI_ENTRIES; i++)
		if (ents[i].cx != NULL && ents[i].dim == dim && ents[i].attno == attno && ents[i].xmax == xmax && strcmp(ents[i].quals, quals) == 0)
			return &ents[i];
	return NULL;
}

bool
sift_semi_cache_get(Oid dim, AttrNumber attno, const char *quals, Snapshot snapshot, MemoryContext into, int64 **set, int *nset)
{
	uint64		xmax;
	SemiEnt    *e;

	if (!settled_xmax(snapshot, &xmax) || (e = ent_find(dim, attno, quals, xmax)) == NULL)
		return false;
	e->used = sift_cache_tick();
	*nset = e->nset;
	*set = NULL;
	if (e->nset > 0)
	{
		*set = MemoryContextAlloc(into, (Size) e->nset * sizeof(int64));
		memcpy(*set, e->set, (Size) e->nset * sizeof(int64));
	}
	return true;
}

void
sift_semi_cache_put(Oid dim, AttrNumber attno, const char *quals, Snapshot snapshot, const int64 *set, int nset)
{
	uint64		xmax;
	Size		need;
	SemiEnt    *e,
				fresh;
	MemoryContext cx;

	if (!settled_xmax(snapshot, &xmax))
		return;
	need = strlen(quals) + 1 + (Size) Max(nset, 0) * sizeof(int64);
	if (!registered)
	{
		sift_cache_register(&semi_kind);
		registered = true;
	}
	if (!sift_cache_room(need + 1024) || ent_find(dim, attno, quals, xmax) != NULL)
		return;
	if ((e = ent_free()) == NULL)
	{
		semi_evict_one();
		e = ent_free();
	}
	cx = sift_cache_create_context_in(CurrentMemoryContext, "sift cache semi set");
	fresh = (SemiEnt) {.cx = cx,.dim = dim,.attno = attno,.xmax = xmax,.nset = nset,.used = sift_cache_tick()};
	fresh.quals = MemoryContextStrdup(cx, quals);
	if (nset > 0)
	{
		fresh.set = MemoryContextAlloc(cx, (Size) nset * sizeof(int64));
		memcpy(fresh.set, set, (Size) nset * sizeof(int64));
	}
	sift_cache_adopt(cx);
	*e = fresh;
	sift_cache_commit();
}
