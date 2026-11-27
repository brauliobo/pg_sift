/*
 * test/unit/table/f2/lock/kept_end.c - the end of a transaction and the committed updates a chunked stripe keeps (table/f2/lock/lock.c, reclaim_kept): an end that touched no row and whose
 * snapshot came after every update kept moves no horizon past them and reads no process array; one whose snapshot is not after them asks the horizon and sweeps the stripe.
 */
#define SIFT_S49_STANDALONE 1

#include "postgres.h"

#undef printf
#undef fprintf
#undef snprintf
#undef qsort

#include "test/common/check.h"

#include "table/f2/lock/lock.c"

sigjmp_buf *PG_exception_stack = NULL;
ErrorContextCallback *error_context_stack = NULL;

enum { KEPT = 1000 };

TransactionId TransactionXmin = FirstNormalTransactionId;
static int	horizons;
static int	sweeps;

uint32
sift_touched_count(void)
{
	return 0;
}

void
sift_touched_reset(void)
{
}

void
sift_esc_xact_end(XactEvent event)
{
}

FullTransactionId
ReadNextFullTransactionId(void)
{
	return FullTransactionIdFromEpochAndXid(0, KEPT + 100);
}

TransactionId
GetOldestNonRemovableTransactionId(Relation rel)
{
	horizons++;
	return KEPT + 50;
}

bool
LWLockAcquire(LWLock *l, LWLockMode mode)
{
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
sift_cache_create_context(const char *name)
{
	return CurrentMemoryContext;
}

dsa_area *
dsa_attach(dsa_handle handle)
{
	return (dsa_area *) rows;
}

void
dsa_pin_mapping(dsa_area *a)
{
}

bool
LWLockConditionalAcquire(LWLock *l, LWLockMode mode)
{
	sweeps++;
	return false;
}

/* The asks of the horizon and the sweeps of stripes of the end of a transaction that touched no row, with snapshot xmin */
static void
end(XactEvent event, TransactionId xmin, int want)
{
	horizons = sweeps = 0;
	TransactionXmin = xmin;
	sift_tbl_rowlock_xact_callback(event, NULL);
	SIFT_CHECKF(horizons == want && sweeps == want, "an end (%d) with xmin %u asks the horizon %d times (%d) and sweeps %d stripes (%d)", event, xmin, horizons, want, sweeps, want);
}

int
main(void)
{
	rows = calloc(1, sizeof(SiftRowLockTable));
	pg_atomic_init_u32(&rows->area_handle, 42);
	rows->hdr[9].overflow = 1;
	sift_rowlock_stripe_acquire(rows, 9, LW_EXCLUSIVE);
	rows->hdr[9].overflow = InvalidDsaPointer;
	SIFT_CHECKF(sift_rowlock_area_mapped(), "the backend maps the area of the chunks");
	pg_atomic_write_u64(&rows->kept[9], U64FromFullTransactionId(FullTransactionIdFromEpochAndXid(0, KEPT)));

	end(XACT_EVENT_COMMIT, KEPT + 10, 0);
	end(XACT_EVENT_ABORT, KEPT + 10, 0);
	end(XACT_EVENT_COMMIT, KEPT, 1);
	end(XACT_EVENT_ABORT, KEPT - 10, 1);
	pg_atomic_write_u64(&rows->kept[9], 0);
	end(XACT_EVENT_COMMIT, KEPT - 10, 0);
	free(rows);
	return sift_check_summary();
}
