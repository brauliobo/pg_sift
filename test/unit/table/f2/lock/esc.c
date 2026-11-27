/*
 * test/unit/table/f2/lock/esc.c - an allocation of the locks of a transaction or of the undo log of its subtransactions that fails (out of memory) leaves them as they were:
 * every lock is found, the rollback of the subtransaction undoes its records, and the end of the transaction reads every lock (S118-04).
 */
#define SIFT_S49_STANDALONE 1

#include "postgres.h"

#undef printf
#undef fprintf
#undef snprintf
#undef qsort

#include "test/common/bench.h"
#include "test/common/oom.h"

#include "table/f2/lock/esc.c"

#undef MemoryContextAlloc
#undef MemoryContextAllocZero
#undef repalloc

enum { SUB = TopSubTransactionId + 1 };

static RelationData rels[65];

MemoryContext
sift_mem_allocset_create(MemoryContext parent, const char *name)
{
	return CurrentMemoryContext;
}

int
GetCurrentTransactionNestLevel(void)
{
	return 2;
}

SubTransactionId
GetCurrentSubTransactionId(void)
{
	return SUB;
}

TransactionId
GetCurrentTransactionId(void)
{
	return 100;
}

TransactionId
GetTopTransactionIdIfAny(void)
{
	return 100;
}

static void
lock_rel(void *arg)
{
	Relation	rel = arg;

	(void) lock_of(rel, SIFT_ESC_PART_ALL);
}

static void
note_rel(void *arg)
{
	note_transition(find_lock(&((Relation) arg)->rd_locator, SIFT_ESC_PART_ALL));
}

static void
abort_sub(void *arg pg_attribute_unused())
{
	sift_esc_subxact_event(SUBXACT_EVENT_ABORT_SUB, SUB, TopSubTransactionId);
}

static void
end_xact(void *arg pg_attribute_unused())
{
	sift_esc_xact_end(XACT_EVENT_ABORT);
}

/* The call on rels[last] with its allocation number fail_at failing, after the ones before it, and then again with none failing */
static void
fail_once(void (*fn) (void *), int last, int fail_at, const char *what)
{
	for (int i = 0; i < last; i++)
		SIFT_CHECK(pgshim_try(fn, &rels[i]));
	oom_after = fail_at;
	SIFT_CHECKF(!pgshim_try(fn, &rels[last]) && pgshim_sqlstate == ERRCODE_OUT_OF_MEMORY, "%s: the allocation fails", what);
	oom_after = -1;
	SIFT_CHECK(pgshim_try(fn, &rels[last]));
}

int
main(void)
{
	for (int i = 0; i < lengthof(rels); i++)
		rels[i].rd_locator = (RelFileLocator) {.spcOid = 1663,.dbOid = 5,.relNumber = 16384 + i};
	fail_once(lock_rel, 64, 1, "the index of the locks");
	for (int i = 0; i < lengthof(rels); i++)
		SIFT_CHECKF(find_lock(&rels[i].rd_locator, SIFT_ESC_PART_ALL) != NULL, "the lock of relation %d is found after the failure", i);
	fail_once(note_rel, 8, 1, "the buckets of the undo log");
	SIFT_CHECKF(nsub == 9, "%d records of the subtransaction", nsub);
	for (int i = 0; i <= 8; i++)
		SIFT_CHECKF(sub_find(SUB, &rels[i].rd_locator, SIFT_ESC_PART_ALL) != NULL, "the record of relation %d is found", i);
	SIFT_CHECKF(pgshim_try(abort_sub, NULL) && nsub == 0, "the rollback of the subtransaction undoes its records: %d left", nsub);
	SIFT_CHECKF(pgshim_try(end_xact, NULL) && nlocks == 0, "the end of the transaction lets every lock go");
	return sift_check_summary();
}
