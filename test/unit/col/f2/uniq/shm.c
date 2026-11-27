/*
 * test/unit/col/f2/uniq/shm.c - a growth of the list of the unique key entries a transaction made that fails (out of memory) leaves the list as it was: the next entry grows it,
 * and is written inside it (S118-04). The end of a subtransaction costs its own entries, not the list (S119-21), and a stripe with no free entry is not swept again before a
 * transaction ends (S119-21).
 */
#define SIFT_S49_STANDALONE 1

#include "postgres.h"

#include "access/transam.h"

#undef printf
#undef fprintf
#undef snprintf
#undef qsort

#include "test/common/bench.h"
#include "test/common/oom.h"

char	   *sift_test_dsm_fail = NULL;
MemoryContext TopTransactionContext = (MemoryContext) 1;
#if PG_VERSION_NUM >= 170000
static TransamVariablesData transam = {.xactCompletionCount = 5};
TransamVariablesData *TransamVariables = &transam;
#else
static VariableCacheData transam = {.xactCompletionCount = 5};
VariableCache ShmemVariableCache = &transam;
#endif
static SubTransactionId cur_sub = TopSubTransactionId;
static int	xid_checks;

#include "col/f2/uniq/shm.c"

#undef MemoryContextAlloc
#undef MemoryContextAllocZero
#undef repalloc

void
before_shmem_exit(pg_on_exit_callback function, Datum arg)
{
}

void
RegisterXactCallback(XactCallback callback, void *arg)
{
}

void
RegisterSubXactCallback(SubXactCallback callback, void *arg)
{
}

SubTransactionId
GetCurrentSubTransactionId(void)
{
	return cur_sub;
}

void
sift_test_phase_wait(int gate)
{
}

bool
LWLockAcquire(LWLock *lock, LWLockMode mode)
{
	return true;
}

void
LWLockRelease(LWLock *lock)
{
}

bool
TransactionIdIsCurrentTransactionId(TransactionId xid)
{
	xid_checks++;
	return true;
}

bool
TransactionIdIsInProgress(TransactionId xid)
{
	return true;
}

static void
ensure(void *arg pg_attribute_unused())
{
	owned_ensure();
}

static void
add(void)
{
	SiftUniqKey k = {.locator = {.spcOid = 1663,.dbOid = 5,.relNumber = 16384},.hash = (uint32) nowned};

	SIFT_CHECK(pgshim_try(ensure, NULL));
	owned_add(&k, (uint32) nowned, 100);
}

static bool
reserve(uint32 hash)
{
	SiftUniqKey k;
	ItemPointerData tid;

	sift_uniq_key_begin(&k, &(RelFileLocator) {.spcOid = 1663,.dbOid = 5,.relNumber = 16385}, hash, 1);
	ItemPointerSet(&tid, 0, 1);
	return sift_uniq_shm_reserve(&k, 100, &tid);
}

/* 4000 subtransactions of one entry each, after n entries of the transaction: every other one commits, the others roll back */
static double
subxacts(int n)
{
	static uint32 next;
	static SubTransactionId sub = TopSubTransactionId;
	double		t0;

	nowned = 0;
	cur_sub = TopSubTransactionId;
	for (int i = 0; i < n; i++)
	{
		ensure(NULL);
		owned_add(&(SiftUniqKey) {.hash = (uint32) i}, SIFT_UNIQ_SHM_SLOTS - 1, 100);
	}
	t0 = now_s();
	for (int i = 0; i < 4000; i++)
	{
		cur_sub = ++sub;
		SIFT_CHECK(reserve(1 + (next++ << STRIPE_BITS) % SIFT_UNIQ_SHM_BUCKETS));
		uniq_subxact(i % 2 ? SUBXACT_EVENT_COMMIT_SUB : SUBXACT_EVENT_ABORT_SUB, cur_sub, TopSubTransactionId, NULL);
		cur_sub = TopSubTransactionId;
	}
	t0 = now_s() - t0;
	SIFT_CHECKF(nowned == n + 2000, "the committed subtransactions pass %d entries to the transaction, %d listed", n + 2000, nowned);
	return t0;
}

/* A stripe whose entries and the spares are all taken asks the process array for its transactions once, until a transaction ends */
static void
full_stripe_sweeps_once(void)
{
	int			checks;

	nowned = 0;
	cur_sub = TopSubTransactionId;
	while (reserve(0))
		;
	checks = xid_checks;
	SIFT_CHECKF(!reserve(0) && xid_checks == checks, "a full stripe is swept again (%d xid checks) though no transaction ended", xid_checks - checks);
	transam.xactCompletionCount++;
	SIFT_CHECKF(!reserve(0) && xid_checks > checks, "a full stripe is swept once a transaction ended");
}

int
main(void)
{
	int			before;

	add();
	while (nowned < capowned)
		add();
	oom_after = 0;
	SIFT_CHECKF(!pgshim_try(ensure, NULL) && pgshim_sqlstate == ERRCODE_OUT_OF_MEMORY, "the growth of the list fails");
	oom_after = -1;
	before = oom_allocs;
	add();
	SIFT_CHECKF(oom_allocs > before, "the entry after the failure grows the list of %d entries", nowned - 1);
	shm = base_of(calloc(1, sizeof(UniqShm) + PG_CACHE_LINE_SIZE));
	report("uniq subxact end", subxacts, 4000, 3.0);
	full_stripe_sweeps_once();
	return sift_check_summary();
}
