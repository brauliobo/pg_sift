/*
 * test/unit/col/f2/shard_io.c - an error in the set-up of a flush of a hot directory or of a promotion (col/f2/l0/shard_io.c), the load of the directory or the reservation of its pages,
 * gives back the entry of the hot partition the backend holds and every page it reserved: a backend that held on to them would starve the other partitions of entries, and stop at
 * the ninth one.
 */
#include "postgres.h"

#include "col/f2/l0/shard_io.c"

#include "test/common/check.h"
#include "test/common/pgshim.h"

sigjmp_buf *PG_exception_stack;
ErrorContextCallback *error_context_stack;

static int	unpins,
			reserved,
			fail_reserve_at = -1;
static bool fail_load;

void
pg_re_throw(void)
{
	siglongjmp(*PG_exception_stack, 1);
}

static void
throw_error(void)
{
	pg_re_throw();
}

void
sift_href_io_init(SiftHrefIo *h, Relation rel, uint32 part_id)
{
	*h = (SiftHrefIo) {.rel = rel,.part_id = part_id};
}

void
sift_href_io_reserve(SiftHrefIo *h, int n)
{
	while (h->npool < n)
	{
		if (h->npool == fail_reserve_at)
			throw_error();
		h->npool++;
		reserved++;
	}
}

void
sift_href_io_unreserve(SiftHrefIo *h)
{
	reserved -= h->npool;
	h->npool = 0;
}

void
sift_l0_shm_load(const SiftL0PageIO *io, SiftL0ShardShm *shm, BlockNumber dir_blk)
{
	if (fail_load)
		throw_error();
}

void
sift_l0_hot_unpin(SiftL0ShardShm *e)
{
	unpins++;
}

FullTransactionId
ReadNextFullTransactionId(void)
{
	return FullTransactionIdFromEpochAndXid(0, 1000);
}

TransactionId
GetOldestNonRemovableTransactionId(Relation rel)
{
	return 900;
}

static SiftL0Table t;
static SiftL0ShardShm entry;

/* fn throws; true when the error reached the test */
static bool
fails(void (*fn) (void))
{
	volatile bool failed = false;

	unpins = reserved = 0;
	PG_TRY();
	{
		fn();
	}
	PG_CATCH();
	{
		failed = true;
	}
	PG_END_TRY();
	fail_load = false;
	fail_reserve_at = -1;
	return failed;
}

static void
flush(void)
{
	HotFlush	hf = {0};

	flush_shared(&hf, NULL, &t, hold(&entry), 7);
}

static void
promote_new(void)
{
	promote(NULL, &entry, 1, NULL, NO_BLOCK, false);
}

static void
test_flush_load_fails(void)
{
	pg_atomic_init_u32(&entry.dir_blk, 3);
	fail_load = true;
	SIFT_CHECK(fails(flush));
	SIFT_CHECKF(nheld == 0 && unpins == 1, "a flush whose directory does not load gives its entry back: %d held, %d unpinned", nheld, unpins);
}

static void
test_flush_reserve_fails(void)
{
	pg_atomic_init_u32(&entry.dir_blk, 7);
	fail_reserve_at = 3;
	SIFT_CHECK(fails(flush));
	SIFT_CHECKF(nheld == 0 && unpins == 1 && reserved == 0, "a flush that cannot reserve its pages gives them and its entry back: %d held, %d reserved", nheld, reserved);
}

static void
test_promote_reserve_fails(void)
{
	fail_reserve_at = 5;
	SIFT_CHECK(fails(promote_new));
	SIFT_CHECKF(reserved == 0, "a promotion that cannot reserve its pages gives them back: %d reserved", reserved);
}

/* More errors than a backend holds entries: every one is given back, none is held at the ninth */
static void
test_many_errors(void)
{
	pg_atomic_init_u32(&entry.dir_blk, 3);
	for (int i = 0; i <= HELD_MAX; i++)
	{
		fail_load = true;
		SIFT_CHECK(fails(flush));
	}
	SIFT_CHECKF(nheld == 0, "no entry stays held: %d", nheld);
}

int
main(void)
{
	pgshim_keep();
#ifndef __SANITIZE_THREAD__
	/* one thread; ThreadSanitizer does not find the jmp_buf of a PG_TRY that the ERROR goes to */
	test_flush_load_fails();
	test_flush_reserve_fails();
	test_promote_reserve_fails();
	if (sift_check_failures == 0)
		test_many_errors();
#endif
	return sift_check_summary();
}
