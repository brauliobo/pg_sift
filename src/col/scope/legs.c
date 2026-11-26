/*
 * col/scope/legs.c - the legs of a scope kept for the catalog state they were found at (col/scope/scope.h)
 */
#include "postgres.h"

#include "access/xact.h"
#include "nodes/nodes.h"
#include "storage/sinval.h"
#include "utils/memutils.h"
#include "utils/rel.h"

#include "core/ring.h"
#include "col/am.h"
#include "col/scope/scope.h"
#include "col/scope/memo.h"
#include "core/cachebudget.h"

/*
 * The legs of a scope (the relations, columns, btrees and predicates its pairs come from) are what the catalogs say, 25,000 instructions for each plan of a statement that reads the
 * scope. They are kept for the catalog state they were found at (SharedInvalidMessageCounter, as in col/v1/track/track.c) and not at all in a transaction with an xid, whose own DDL is not counted
 * before it commits; the predicates are copied out of the memo, which the caller may keep.
 */
#define LEGS_MEMO_N 4
static struct LegsMemo
{
	SiftScopeMemoSlot memo;
	Oid			index;
	int			scope;
	uint64		gen;
	int			n;
	SiftScopeLeg legs[2];
} legs_memo[LEGS_MEMO_N];
static SiftRing legs_memo_ring;

static uint64
legs_oldest(void)
{
	return sift_scope_memo_oldest(legs_memo, sizeof(legs_memo[0]), LEGS_MEMO_N);
}

static bool
legs_evict_one(void)
{
	return sift_scope_memo_evict_oldest(legs_memo, sizeof(legs_memo[0]), LEGS_MEMO_N);
}

static void
legs_evict_all(void)
{
	while (legs_evict_one())
		;
}

static const SiftCacheKind legs_kind = {"col_scope_legs", legs_oldest, legs_evict_one, legs_evict_all};

static void
legs_copy(SiftScopeLeg to[2], const SiftScopeLeg from[2], int n)
{
	for (int i = 0; i < n; i++)
	{
		to[i] = from[i];
		to[i].pred = copyObject(from[i].pred);
		to[i].via_pred = copyObject(from[i].via_pred);
	}
}

int
sift_scope_legs(Relation index, int scope, SiftScopeLeg legs[2])
{
	static bool registered;
	bool		keep = !TransactionIdIsValid(GetTopTransactionIdIfAny());
	uint64		gen = SharedInvalidMessageCounter;
	struct LegsMemo *m;
	int			n;
	MemoryContext old;

	for (int i = 0; keep && i < LEGS_MEMO_N; i++)
		if (legs_memo[i].index == RelationGetRelid(index) && legs_memo[i].scope == scope && legs_memo[i].gen == gen)
		{
			sift_catalog_memo_hits++;
			legs_memo[i].memo.used = sift_cache_tick();
			legs_copy(legs, legs_memo[i].legs, legs_memo[i].n);
			return legs_memo[i].n;
		}
	memset(legs, 0, 2 * sizeof(SiftScopeLeg));
	n = sift_scope_legs_find(index, scope, legs);
	if (!keep)
		return n;
	sift_scope_memo_register(&registered, &legs_kind);
	m = &legs_memo[SIFT_RING_SLOT(legs_memo_ring, LEGS_MEMO_N)];
	old = MemoryContextSwitchTo(sift_scope_memo_begin(&m->memo, "sift cache scope legs"));
	legs_copy(m->legs, legs, n);
	MemoryContextSwitchTo(old);
	m->index = RelationGetRelid(index);
	m->scope = scope;
	m->gen = gen;
	m->n = n;
	return n;
}
