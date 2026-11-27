/*
 * test/unit/col/dictshm.c - the readers of the ring of shared dictionaries of col/dictshm/dictshm.c that fail (ERROR) where the ring is not yet attached in their backend: the
 * record found in the directory is not pinned for ever, and the ring stays free to be overwritten; a reader that misses in a server that keeps no dictionary yet does not make the ring.
 */
#include "postgres.h"

#undef printf
#undef fprintf
#undef snprintf
#undef qsort

#include "test/common/bench.h"

#include "col/dictshm/dictshm.c"

#include "test/unit/core/shm/stub_lwlock.h"

char	   *sift_test_dsm_fail;
int			dynamic_shared_memory_type = DSM_IMPL_MMAP;
int64		sift_stat_v[SIFT_STAT_COUNT];
volatile uint32 InterruptHoldoffCount;
ErrorContextCallback *error_context_stack;
sigjmp_buf *PG_exception_stack;

static void *ring_seg;
static int	ring_asks;
static bool ring_fails;
static MemoryContextCallback *reset_cb;

/* What errfinish does in the server: the error goes to the innermost PG_TRY */
void
pg_re_throw(void)
{
	siglongjmp(*PG_exception_stack, 1);
}

Size
sift_shm_pool_bytes(SiftShmPoolId pool)
{
	return 1 << 20;
}

/* The ring is the one segment the server has for its name; an attach can fail (an ERROR the caller does not catch: a parallel worker, a backend outside a transaction) */
void *
sift_dsm_attach(const char *name, Size size, SiftDsmInit init, bool *failed, const char *what, const char *setting, bool fail)
{
	if (strcmp(name, DSHM_RING_NAME) != 0)
		return stub_segment(size, init);
	ring_asks++;
	if (ring_fails)
	{
		ring_fails = false;
		pg_re_throw();
	}
	if (ring_seg == NULL)
		ring_seg = stub_segment(size, init);
	return ring_seg;
}

void
sift_stats_share(SiftStat s, int64 n)
{
}

void
sift_shm_note_eviction(SiftShmPoolId pool, uint64 count)
{
}

bool
sift_shm_first(const char *name, const char *reason)
{
	return false;
}

bool
IsInParallelMode(void)
{
	return false;
}

void
before_shmem_exit(pg_on_exit_callback function, Datum arg)
{
}

SiftCDict *
sift_cdict_create(MemoryContext mcxt)
{
	SiftCDict  *d = palloc0(sizeof(SiftCDict));

	d->mcxt = mcxt;
	d->off = palloc0(sizeof(uint32));
	d->bytes = palloc0(1);
	return d;
}

void
MemoryContextRegisterResetCallback(MemoryContext context, MemoryContextCallback *cb)
{
	reset_cb = cb;
}

static DictKey
key_of(uint32 unit)
{
	DictKey		k;

	memset(&k, 0, sizeof(k));
	k.rel = (RelFileLocator) {.spcOid = 1663,.dbOid = 5,.relNumber = 16384};
	k.gen = 1;
	k.ndocs = 2;
	k.unit = unit;
	return k;
}

static uint32
pins_of(const DictKey *k)
{
	uint32		n = 0;

	for (int i = 0; i < lengthof(dshm->dir); i++)
		if (dshm->dir[i].state != E_FREE && memcmp(&dshm->dir[i].key, k, sizeof(*k)) == 0)
			n += pg_atomic_read_u32(&dshm->dir[i].pins);
	return n;
}

static void
keep(const DictKey *k)
{
	uint32		off[3] = {0, 3, 6};
	SiftCDict	d = {.n = 2,.off = off,.bytes = "abcdef",.nbytes = 6};

	SIFT_CHECK(sift_dictshm_write(k, &d));
}

/* Whether the read raised an ERROR, caught as the caller of the code under test would catch it */
static bool
read_raises(const DictKey *k)
{
	sigjmp_buf	jb;
	sigjmp_buf *outer = PG_exception_stack;

	if (sigsetjmp(jb, 0) != 0)
	{
		PG_exception_stack = outer;
		return true;
	}
	PG_exception_stack = &jb;
	(void) sift_dictshm_read(k, CurrentMemoryContext);
	PG_exception_stack = outer;
	return false;
}

/* A backend that keeps nothing and misses does not make the ring */
static void
test_miss_makes_no_ring(void)
{
	DictKey		k = key_of(1);

	SIFT_CHECK(sift_dictshm_read(&k, CurrentMemoryContext) == NULL);
	SIFT_CHECKF(ring_asks == 0 && !dshm->ring_ready, "a read that missed made the ring (%d asks)", ring_asks);
}

/* Another backend kept the record; this one, with the ring not yet attached, fails to attach it: the record is not pinned, and a reader that attaches it reads and gives it back */
static void
test_failed_attach_leaves_no_pin(void)
{
	DictKey		k = key_of(2);
	SiftCDict  *d;

	keep(&k);
	ring = NULL;
	ring_fails = true;
	SIFT_CHECKF(read_raises(&k), "the attach of the ring fails");
	SIFT_CHECKF(pins_of(&k) == 0, "a reader that failed to attach the ring holds %u pins on the record for ever", pins_of(&k));
	d = sift_dictshm_read(&k, CurrentMemoryContext);
	SIFT_CHECKF(d != NULL && d->n == 2 && memcmp(d->bytes, "abcdef", 6) == 0 && pins_of(&k) == 1, "the next reader reads the record in place and pins it");
	reset_cb->func(reset_cb->arg);
	SIFT_CHECKF(pins_of(&k) == 0, "the context of the scan gives the pin back");
}

int
main(void)
{
	sift_test_col_dict_shared = 1;
	SIFT_CHECK(dshm_get() != NULL);
	test_miss_makes_no_ring();
#ifndef __SANITIZE_THREAD__
	/* one thread; ThreadSanitizer does not find the jmp_buf the ERROR goes to */
	test_failed_attach_leaves_no_pin();
#endif
	return sift_check_summary();
}
