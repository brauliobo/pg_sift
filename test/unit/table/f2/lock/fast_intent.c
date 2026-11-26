/*
 * test/unit/table/f2/lock/fast_intent.c - a coarse lock reads the slot of a fast intention of another process as one whole: while its owner moves the slot from a transaction of one
 * relation to a transaction of another, sift_fast_intent_conflict for the first relation never returns the xid of the second (a wait on a transaction that holds nothing of the key).
 */
#define SIFT_S49_STANDALONE 1

#include "postgres.h"

#undef printf
#undef fprintf
#undef snprintf
#undef qsort

#include "test/common/bench.h"

#include "table/f2/lock/fast_intent.c"

#if defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__)
enum { ROUNDS = 200000 };
#else
enum { ROUNDS = 2000000 };
#endif
enum { XID_T1 = 100, XID_T2 = 200 };

static const RelFileLocator t1 = {.spcOid = 1663,.dbOid = 5,.relNumber = 16384};
static const RelFileLocator t2 = {.spcOid = 1663,.dbOid = 5,.relNumber = 16385};
static SiftRowLockShmLayout *shm;
static volatile TransactionId top_xid;
static pg_atomic_uint32 done;
static pg_atomic_uint32 torn;

int			MaxBackends = 2;
ProcNumber	MyProcNumber;

/* An exclusive coarse lock is asked for: every mode a holder has conflicts with it */
bool
DoLockModesConflict(LOCKMODE mode1, LOCKMODE mode2)
{
	return mode1 != NoLock && mode2 != NoLock;
}

uint32
sift_esc_stripe_of(const RelFileLocator *loc, uint64 part_id)
{
	return 0;
}

TransactionId
GetTopTransactionId(void)
{
	return top_xid;
}

bool
TransactionIdIsCurrentTransactionId(TransactionId xid)
{
	return false;
}

bool
TransactionIdIsInProgress(TransactionId xid)
{
	return true;
}

/* Worker 0 owns proc 0: a slot of t1 for xid 100, freed, then the same slot of t2 for xid 200; worker 1 asks for a coarse lock of t1 */
static void
worker(void *arg pg_attribute_unused(), int w)
{
	SiftEscModes want = sift_esc_coarse_modes(LockTupleExclusive);

	if (w == 0)
	{
		for (int r = 0; r < ROUNDS; r++)
		{
			const RelFileLocator *loc = r % 2 == 0 ? &t1 : &t2;

			top_xid = r % 2 == 0 ? XID_T1 : XID_T2;
			SIFT_CHECK(sift_fast_intent_set(shm, loc, SIFT_ESC_PART_ALL, -1, LockTupleExclusive, 0) == 0);
			sift_fast_intent_restore(shm, 0, -1);
		}
		pg_atomic_write_u32(&done, 1);
		return;
	}
	while (pg_atomic_read_u32(&done) == 0)
		if (sift_fast_intent_conflict(shm, &t1, SIFT_ESC_PART_ALL, want) == XID_T2)
			pg_atomic_fetch_add_u32(&torn, 1);
}

int
main(void)
{
	shm = calloc(1, sizeof(SiftRowLockShmLayout) + sift_fast_intent_bytes() + PG_CACHE_LINE_SIZE);
	sift_fast_intent_init(shm);
	MyProcNumber = 0;
	pg_atomic_init_u32(&done, 0);
	pg_atomic_init_u32(&torn, 0);
	unit_run_workers(2, worker, NULL);
	SIFT_CHECKF(pg_atomic_read_u32(&torn) == 0, "a coarse lock of t1 read the xid of t2 out of a slot that moved: %u times", pg_atomic_read_u32(&torn));
	free(shm);
	return sift_check_summary();
}
