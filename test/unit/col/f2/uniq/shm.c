/*
 * test/unit/col/f2/uniq/shm.c - a growth of the list of the unique key entries a transaction made that fails (out of memory) leaves the list as it was: the next entry grows it,
 * and is written inside it (S118-04).
 */
#define SIFT_S49_STANDALONE 1

#include "postgres.h"

#undef printf
#undef fprintf
#undef snprintf
#undef qsort

#include "test/common/bench.h"
#include "test/common/oom.h"

char	   *sift_test_dsm_fail = NULL;
MemoryContext TopTransactionContext = (MemoryContext) 1;

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
	return TopSubTransactionId;
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
	return sift_check_summary();
}
