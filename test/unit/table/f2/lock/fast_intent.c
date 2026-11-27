/*
 * test/unit/table/f2/lock/fast_intent.c - a coarse lock reads the slot of a fast intention of another process as one whole: while its owner moves the slot from a transaction of one
 * relation to a transaction of another, sift_fast_intent_conflict for the first relation never returns the xid of the second (a wait on a transaction that holds nothing of the key).
 * A coarse lock among many processes reads the slots that are held, not the free ones of every process, and takes a cancel while it scans them.
 * A process records the relation and partition intentions of 32 sift_tables (S119-28: a transaction that locked a row in each of 17 took page locks), and a coarse lock reads each of them.
 * A coarse lock of a partition meets the intention of another partition that shares its page lock (S120-16: PREPARE of the holder of the intention found that lock taken), and the intention
 * taken after the coarse lock of the other partition is refused as a record.
 */
#define SIFT_S49_STANDALONE 1

#include "postgres.h"

#undef printf
#undef fprintf
#undef snprintf
#undef qsort

#include "test/common/bench.h"

#include "port/atomics.h"

static uint64 slot_reads;

#undef pg_read_barrier
#define pg_read_barrier() do { __atomic_fetch_add(&slot_reads, 1, __ATOMIC_RELAXED); pg_read_barrier_impl(); } while (0)

#include "table/f2/lock/fast_intent.c"

#if defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__)
enum { ROUNDS = 200000 };
#else
enum { ROUNDS = 2000000 };
#endif
enum { XID_T1 = 100, XID_T2 = 200, MANY = 256, HOLDERS = 64, TABLES = 32 };

static const RelFileLocator t1 = {.spcOid = 1663,.dbOid = 5,.relNumber = 16384};
static const RelFileLocator t2 = {.spcOid = 1663,.dbOid = 5,.relNumber = 16385};
static SiftRowLockShmLayout *shm;
static volatile TransactionId top_xid;
static pg_atomic_uint32 done;
static pg_atomic_uint32 torn;

int			MaxBackends = 2;
ProcNumber	MyProcNumber;
volatile sig_atomic_t InterruptPending = 0;
static int	interrupts;

void
ProcessInterrupts(void)
{
	interrupts++;
}

/* An exclusive coarse lock is asked for: every mode a holder has conflicts with it */
bool
DoLockModesConflict(LOCKMODE mode1, LOCKMODE mode2)
{
	return mode1 != NoLock && mode2 != NoLock;
}

uint32
sift_esc_stripe_of(const RelFileLocator *loc, uint64 part_id)
{
	return (uint32) (part_id % SIFT_ESC_STRIPES);
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

/* HOLDERS of MANY processes hold an intention of t2, the others none: a coarse lock of t1 reads the held slots, and checks for a cancel for each process */
static void
test_scan_reads_held_slots(void)
{
	SiftEscModes want = sift_esc_coarse_modes(LockTupleExclusive);

	MaxBackends = MANY;
	shm = calloc(1, sizeof(SiftRowLockShmLayout) + sift_fast_intent_bytes() + PG_CACHE_LINE_SIZE);
	sift_fast_intent_init(shm);
	for (int h = 0; h < HOLDERS; h++)
	{
		MyProcNumber = h * (MANY / HOLDERS);
		top_xid = XID_T2 + h;
		SIFT_CHECK(sift_fast_intent_set(shm, &t2, SIFT_ESC_PART_ALL, -1, LockTupleExclusive, 0) == 0);
	}
	slot_reads = 0;
	InterruptPending = 1;
	SIFT_CHECK(sift_fast_intent_conflict(shm, &t1, SIFT_ESC_PART_ALL, want) == InvalidTransactionId);
	InterruptPending = 0;
	SIFT_CHECKF(slot_reads <= HOLDERS, "a coarse lock read %llu slots for %d held ones of %d processes", (unsigned long long) slot_reads, HOLDERS, nprocs());
	SIFT_CHECKF(interrupts >= HOLDERS, "a scan of %d processes checked for a cancel %d times", nprocs(), interrupts);
	for (int h = 0; h < HOLDERS; h++)
	{
		MyProcNumber = h * (MANY / HOLDERS);
		sift_fast_intent_release_all(shm);
	}
	SIFT_CHECK(pg_atomic_read_u32(fast_active(shm, &t2, SIFT_ESC_PART_ALL)) == 0);
	free(shm);
}

/* One process locks a row in each of TABLES relations: both intentions of each are records, the last slot's is seen by a coarse lock, and a slot past them is refused */
static void
test_slots_of_many_tables(void)
{
	SiftEscModes want = sift_esc_coarse_modes(LockTupleExclusive);
	RelFileLocator loc = t1;

	MaxBackends = 2;
	MyProcNumber = 0;
	top_xid = XID_T1;
	shm = calloc(1, sizeof(SiftRowLockShmLayout) + sift_fast_intent_bytes() + PG_CACHE_LINE_SIZE);
	sift_fast_intent_init(shm);
	for (int t = 0; t < TABLES; t++)
	{
		loc.relNumber = t1.relNumber + t;
		SIFT_CHECKF(sift_fast_intent_set(shm, &loc, SIFT_ESC_PART_ALL, -1, LockTupleKeyShare, 0) == 2 * t, "the relation intention of table %d is a record", t + 1);
		SIFT_CHECKF(sift_fast_intent_set(shm, &loc, 1, -1, LockTupleKeyShare, 0) == 2 * t + 1, "the partition intention of table %d is a record", t + 1);
	}
	SIFT_CHECK(sift_fast_intent_conflict(shm, &loc, 1, want) == XID_T1);
	loc.relNumber++;
	SIFT_CHECK(sift_fast_intent_set(shm, &loc, SIFT_ESC_PART_ALL, -1, LockTupleKeyShare, 0) == -1);
	sift_fast_intent_release_all(shm);
	SIFT_CHECK(sift_fast_intent_conflict(shm, &t1, SIFT_ESC_PART_ALL, want) == InvalidTransactionId);
	free(shm);
}

/* Partitions P and P + SIFT_LOCK_PART_ESC_SLOTS are one page lock of the lock manager: an intention of one is in the way of a coarse lock of the other, a partition next to them is not */
static void
test_partitions_that_share_a_page_lock(void)
{
	SiftEscModes want = sift_esc_coarse_modes(LockTupleExclusive);
	uint64		p1 = 7,
				p2 = p1 + SIFT_LOCK_PART_ESC_SLOTS;

	MaxBackends = 2;
	MyProcNumber = 0;
	top_xid = XID_T1;
	shm = calloc(1, sizeof(SiftRowLockShmLayout) + sift_fast_intent_bytes() + PG_CACHE_LINE_SIZE);
	sift_fast_intent_init(shm);
	SIFT_CHECK(sift_fast_intent_set(shm, &t1, p1, -1, LockTupleKeyShare, 0) == 0);
	SIFT_CHECKF(sift_fast_intent_conflict(shm, &t1, p2, want) == XID_T1, "a coarse lock of partition %llu meets the intention of partition %llu", (unsigned long long) p2, (unsigned long long) p1);
	SIFT_CHECK(sift_fast_intent_conflict(shm, &t1, p1 + 1, want) == InvalidTransactionId);
	sift_fast_intent_release_all(shm);
	sift_esc_coarse_enter(shm, &t1, p2);
	MyProcNumber = 1;
	SIFT_CHECKF(sift_fast_intent_set(shm, &t1, p1, -1, LockTupleKeyShare, 0) == -1, "an intention of partition %llu under a coarse lock of partition %llu takes the page lock", (unsigned long long) p1, (unsigned long long) p2);
	sift_esc_coarse_leave(shm, &t1, p2);
	free(shm);
}

int
main(void)
{
	test_partitions_that_share_a_page_lock();
	test_slots_of_many_tables();
	shm = calloc(1, sizeof(SiftRowLockShmLayout) + sift_fast_intent_bytes() + PG_CACHE_LINE_SIZE);
	sift_fast_intent_init(shm);
	MyProcNumber = 0;
	pg_atomic_init_u32(&done, 0);
	pg_atomic_init_u32(&torn, 0);
	unit_run_workers(2, worker, NULL);
	SIFT_CHECKF(pg_atomic_read_u32(&torn) == 0, "a coarse lock of t1 read the xid of t2 out of a slot that moved: %u times", pg_atomic_read_u32(&torn));
	free(shm);
	test_scan_reads_held_slots();
	return sift_check_summary();
}
