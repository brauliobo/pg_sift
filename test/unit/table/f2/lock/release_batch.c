/*
 * test/unit/table/f2/lock/release_batch.c - a subtransaction that escalates gives back its rows of tier 1 a window at a time, each stripe locked once for the rows of the window that are in it, and
 * takes a cancel between windows (S120-12: a release of 32768 rows took 32768 exclusive stripe locks with no check for a cancel); every row it held goes, and a speculative insert in flight stays.
 */
#define SIFT_S49_STANDALONE 1

#include "postgres.h"

#undef printf
#undef fprintf
#undef snprintf
#undef qsort

#include "common/hashfn.h"
#include "test/common/check.h"

#include "table/f2/lock/lock.c"

enum { ROWS = 3 * SETTLE_WINDOW, XID = 700, SUB = 2 };

sigjmp_buf *PG_exception_stack = NULL;
ErrorContextCallback *error_context_stack = NULL;
volatile sig_atomic_t InterruptPending = 0;
static int	stripe_locks;
static int	interrupts;
static int	released;

void
ProcessInterrupts(void)
{
	interrupts++;
}

bool
LWLockAcquire(LWLock *l, LWLockMode mode)
{
	stripe_locks++;
	return true;
}

void
LWLockRelease(LWLock *l)
{
}

bool
IsTransactionState(void)
{
	return true;
}

MemoryContext
sift_rowlock_txn_cx(void)
{
	return CurrentMemoryContext;
}

TransactionId
GetCurrentTransactionId(void)
{
	return XID;
}

SubTransactionId
GetCurrentSubTransactionId(void)
{
	return SUB;
}

void
sift_spec_end(SiftRowLockEntry *e, TransactionId xid)
{
}

void
sift_esc_note_tier1_release(const RelFileLocator *loc, uint64 part_id, uint32 n)
{
	released += n;
}

static SiftRowLockKey
key_n(uint64 n)
{
	return (SiftRowLockKey) {.locator = {.spcOid = 1663,.dbOid = 5,.relNumber = 16384},.tid = n << 16 | 1};
}

int
main(void)
{
	RelFileLocator loc = key_n(0).locator;
	uint32		held = 0;

	rows = calloc(1, sizeof(SiftRowLockTable));
	for (int si = 0; si < SIFT_ROWLOCK_PARTITIONS; si++)
		rows->hdr[si].overflow = InvalidDsaPointer;
	for (uint64 n = 0; n < ROWS; n++)
	{
		SiftRowLockKey k = key_n(n);
		int			si = hash_bytes((const unsigned char *) &k, sizeof(k)) % SIFT_ROWLOCK_PARTITIONS;
		SiftRowLockEntry *e;

		if (!sift_rowlock_has_room(rows, si))
			continue;
		e = sift_rowlock_add(rows, si, &k);
		e->nholders = 1;
		e->holders[0] = (SiftRowLockHolder) {XID, SUB};
		e->flags = n == 7 ? SIFT_ROWLOCK_SPECULATIVE : 0;
		SIFT_CHECK(sift_touched_add(&k, 1));
		held++;
	}
	stripe_locks = 0;
	InterruptPending = 1;
	sift_rowlock_release_tier1(&loc, SIFT_ESC_PART_ALL, SUB);
	InterruptPending = 0;
	SIFT_CHECKF(sift_touched_count() == 1 && released == (int) held - 1, "the %u rows but the speculative insert go: %u left, %d released", held, sift_touched_count(), released);
	SIFT_CHECKF(stripe_locks <= 3 * SIFT_ROWLOCK_PARTITIONS, "%u rows in %d windows take %d stripe locks, at most one a stripe a window", held, 3, stripe_locks);
	SIFT_CHECKF(interrupts >= 3, "a release of %d windows checks for a cancel %d times", 3, interrupts);
	for (int si = 0; si < SIFT_ROWLOCK_PARTITIONS; si++)
		for (int i = 0; i < rows->hdr[si].table_n; i++)
			SIFT_CHECKF(rows->stripes[si].entries[i].nholders == (rows->stripes[si].entries[i].key.tid == key_n(7).tid), "an entry of stripe %d holds nothing but the speculative insert", si);
	sift_touched_reset();
	free(rows);
	return sift_check_summary();
}
