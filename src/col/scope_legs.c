/*
 * col/scope_legs.c - the legs of a scope kept for the catalog state they were found at (col/scope.h)
 */
#include "postgres.h"

#include "access/xact.h"
#include "nodes/nodes.h"
#include "storage/sinval.h"
#include "utils/memutils.h"
#include "utils/rel.h"

#include "col/am.h"
#include "col/scope.h"

/*
 * The legs of a scope (the relations, columns, btrees and predicates its pairs come from) are what the catalogs say, 25,000 instructions for each plan of a statement that reads the
 * scope. They are kept for the catalog state they were found at (SharedInvalidMessageCounter, as in col/track.c) and not at all in a transaction with an xid, whose own DDL is not counted
 * before it commits; the predicates are copied out of the memo, which the caller may keep.
 */
#define LEGS_MEMO_N 4
static struct LegsMemo
{
	Oid			index;
	int			scope;
	uint64		gen;
	int			n;
	SiftScopeLeg legs[2];
	MemoryContext cx;
} legs_memo[LEGS_MEMO_N];
static int	legs_memo_next;

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
	bool		keep = !TransactionIdIsValid(GetTopTransactionIdIfAny());
	uint64		gen = SharedInvalidMessageCounter;
	struct LegsMemo *m;
	int			n;
	MemoryContext old;

	for (int i = 0; keep && i < LEGS_MEMO_N; i++)
		if (legs_memo[i].index == RelationGetRelid(index) && legs_memo[i].scope == scope && legs_memo[i].gen == gen)
		{
			sift_catalog_memo_hits++;
			legs_copy(legs, legs_memo[i].legs, legs_memo[i].n);
			return legs_memo[i].n;
		}
	memset(legs, 0, 2 * sizeof(SiftScopeLeg));
	n = sift_scope_legs_find(index, scope, legs);
	if (!keep)
		return n;
	m = &legs_memo[legs_memo_next++ % LEGS_MEMO_N];
	if (m->cx == NULL)
		m->cx = AllocSetContextCreate(TopMemoryContext, "sift cache scope legs", ALLOCSET_SMALL_SIZES);
	MemoryContextReset(m->cx);
	old = MemoryContextSwitchTo(m->cx);
	legs_copy(m->legs, legs, n);
	MemoryContextSwitchTo(old);
	m->index = RelationGetRelid(index);
	m->scope = scope;
	m->gen = gen;
	m->n = n;
	return n;
}
