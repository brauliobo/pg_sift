/*
 * test/unit/table/f2/lock/ovf.c - the area of the row lock chunks is attached with no stripe lock held (an attach takes the lock of the dynamic shared memory of the server and maps a
 * segment): a read of a stripe that has chunks, by a backend that has not mapped the area made by another, maps it before the stripe is locked and finds its entries.
 */
#define SIFT_S49_STANDALONE 1

#include "postgres.h"

#undef printf
#undef fprintf
#undef snprintf
#undef qsort

#include "test/common/bench.h"

char	   *sift_test_dsm_fail = NULL;

#include "table/f2/lock/ovf.c"

extern void sift_rowlock_stripe_acquire(SiftRowLockTable *t, int si, LWLockMode mode);

sigjmp_buf *PG_exception_stack = NULL;
ErrorContextCallback *error_context_stack = NULL;

static SiftRowLockTable *tbl;
static SiftRowLockChunk chunk;
static int	stripes_held;
static int	attaches;
static int	attaches_locked;

static bool
is_stripe(LWLock *l)
{
	return l >= &tbl->hdr[0].lock && l <= &tbl->hdr[SIFT_ROWLOCK_PARTITIONS - 1].lock;
}

bool
LWLockAcquire(LWLock *l, LWLockMode mode)
{
	stripes_held += is_stripe(l);
	return true;
}

void
LWLockRelease(LWLock *l)
{
	stripes_held -= is_stripe(l);
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
	attaches++;
	attaches_locked += stripes_held > 0;
	return (dsa_area *) &chunk;
}

void
dsa_pin_mapping(dsa_area *a)
{
}

void *
dsa_get_address(dsa_area *a, dsa_pointer p)
{
	return &chunk;
}

int
main(void)
{
	SiftRowLockKey k = {.locator = {.spcOid = 1663,.dbOid = 5,.relNumber = 16384},.tid = 7 << 16 | 1};
	int			si = 3;

	tbl = calloc(1, sizeof(SiftRowLockTable));
	pg_atomic_init_u32(&tbl->area_handle, 42);
	tbl->hdr[si].table_n = SIFT_ROWLOCK_BUCKETS;
	tbl->hdr[si].overflow = 1;
	chunk.next = InvalidDsaPointer;
	chunk.n = 1;
	chunk.entries[0].key = k;

	LWLockAcquire(&tbl->hdr[si].lock, LW_EXCLUSIVE);
	(void) sift_rowlock_find(tbl, si, &k);
	(void) sift_rowlock_has_room(tbl, si);
	LWLockRelease(&tbl->hdr[si].lock);
	SIFT_CHECKF(attaches_locked == 0, "a read under the stripe lock attaches no area: %d attaches under it", attaches_locked);

	sift_rowlock_stripe_acquire(tbl, si, LW_EXCLUSIVE);
	SIFT_CHECKF(sift_rowlock_find(tbl, si, &k) == &chunk.entries[0], "the stripe locked for a read finds the entry in its chunk");
	LWLockRelease(&tbl->hdr[si].lock);
	SIFT_CHECKF(attaches == 1 && attaches_locked == 0, "the area is attached once, with no stripe lock held: %d attaches, %d under a stripe lock", attaches, attaches_locked);
	free(tbl);
	return sift_check_summary();
}
