/*
 * test/unit/col/leafshm.c - the writers of the shared statistics of col/leafshm.c that come between the two halves of a write (the place in the ring taken, the directory not yet entered): the
 * directory keeps one entry for an index and kind, the one of the newest stamp, and the entry of another index that took the place the first writer chose is not thrown out for it. A writer
 * that wraps the ring onto the place another writer still copies its record into does not take it: no entry of the directory points to bytes another record is copied over. A writer whose
 * note of the evictions fails (ERROR) has given its place back first: the ring keeps no place held for a copy that is not happening.
 */
#define SIFT_S49_STANDALONE 1

#include "postgres.h"

#undef printf
#undef fprintf
#undef snprintf
#undef qsort

#include "test/common/bench.h"

#include "col/leafshm.c"

#include "test/unit/core/shm/stub_lwlock.h"

Oid			MyDatabaseId = 5;
sigjmp_buf *PG_exception_stack;

/* What errfinish does in the server: the error goes to the innermost PG_TRY */
void
pg_re_throw(void)
{
	siglongjmp(*PG_exception_stack, 1);
}

enum { REL = 16384 };

static RelFileNumber other_rel;
static Size pool_bytes = 1 << 20;
static bool note_fails;
static void (*in_copy) (void);

Size
sift_shm_pool_bytes(SiftShmPoolId pool)
{
	return pool_bytes;
}

void *
sift_dsm_attach(const char *name, Size size, SiftDsmInit init, bool *failed, const char *what, const char *setting, bool fail)
{
	return stub_segment(size, init);
}

static void
pre_write(RelFileNumber rel, uint64 stamp)
{
	SiftColPreStats pre = {0};

	sift_col_leafshm_pre_write(rel, false, stamp, &pre);
}

/* The note of the evictions attaches the ledger of the shared memory the first time, which can fail */
void
sift_shm_note_eviction(SiftShmPoolId pool, uint64 count)
{
	if (note_fails)
	{
		note_fails = false;
		pg_re_throw();
	}
}

/* in_copy runs where the writer has copied its record and comes back for the write lock: the next acquire of it after the first one */
static void
copy_done(void)
{
	stub_hook_lock = &lshm->write_lock.lock;
	stub_hook = in_copy;
}

static void
write_copying(void (*fn) (void))
{
	in_copy = fn;
	stub_hook_lock = &lshm->write_lock.lock;
	stub_hook = copy_done;
}

/* Between its two halves, the first writer is overtaken: another index of its bucket enters the directory, then a newer record of the same index */
static void
overtake(void)
{
	pre_write(other_rel, 9);
	pre_write(REL, 2);
}

static void
entries(RelFileNumber rel, int *n, uint64 *stamp)
{
	*n = 0;
	for (int i = 0; i < LSHM_DIR; i++)
		if (lshm->dir[i].used && lshm->dir[i].rel == rel && lshm->dir[i].kind == LSHM_PRE)
		{
			(*n)++;
			*stamp = lshm->dir[i].stamp;
		}
}

static void
test_overtaken_writer(void)
{
	int			n;
	uint64		stamp = 0;

	other_rel = REL + 1;
	while (lshm_bucket(other_rel) != lshm_bucket(REL))
		other_rel++;
	SIFT_CHECK(lshm_get() != NULL);
	write_copying(overtake);
	pre_write(REL, 1);
	SIFT_CHECKF(stub_hook == NULL, "the other writers came between the two halves");
	entries(REL, &n, &stamp);
	SIFT_CHECKF(n == 1 && stamp == 2, "the index has %d entries of its kind, the last of stamp %llu: one of stamp 2 is wanted", n, (unsigned long long) stamp);
	entries(other_rel, &n, &stamp);
	SIFT_CHECKF(n == 1, "the other index of the bucket has %d entries", n);
}

/* A record whose counts are its index, so that the bytes an entry points to say whose record they are */
static void
marked_write(RelFileNumber rel)
{
	SiftColPreStats pre;

	memset(&pre, (int) (rel & 0x7F), sizeof(pre));
	sift_col_leafshm_pre_write(rel, false, 1, &pre);
}

/* Every entry of the directory points to the whole record of its own index */
static void
entries_whole(void)
{
	for (int i = 0; i < LSHM_DIR; i++)
	{
		const LshmEnt *e = &lshm->dir[i];
		const char *rec = lshm->data + e->off;

		if (!e->used)
			continue;
		SIFT_CHECKF(record_whole(rec, e->len, e->kind, (const LshmHdr *) rec) && rec[sizeof(LshmHdr)] == (char) (e->rel & 0x7F),
					"the entry of index %u points to the bytes of another record", e->rel);
	}
}

static void
wrapping_writer(void)
{
	marked_write(REL + 101);
	entries_whole();
}

/* The ring holds one record and a half: a second writer wraps onto the place of the first one while it copies */
static void
test_wrap_onto_copy(void)
{
	pool_bytes = record_size(LSHM_PRE, 0, 0, 0) * 3 / 2;
	free(lshm);
	lshm = NULL;
	SIFT_CHECK(lshm_get() != NULL);
	write_copying(wrapping_writer);
	marked_write(REL + 100);
	SIFT_CHECKF(stub_hook == NULL, "the second writer came between and the directory was looked at before the first one entered it");
	entries_whole();
	free(lshm);
	lshm = NULL;
}

/* Whether a write raised an ERROR, caught as the caller of the code under test would catch it */
static bool
write_raises(RelFileNumber rel)
{
	sigjmp_buf	jb;
	sigjmp_buf *outer = PG_exception_stack;

	if (sigsetjmp(jb, 0) != 0)
	{
		PG_exception_stack = outer;
		return true;
	}
	PG_exception_stack = &jb;
	marked_write(rel);
	PG_exception_stack = outer;
	return false;
}

/* The writer whose note of the evictions fails has given back the place of its copy: every flight is free, and the place is taken by the next writer */
static void
test_failed_note_frees_flight(void)
{
	int			n;
	uint64		stamp = 0;

	pool_bytes = 1 << 20;
	SIFT_CHECK(lshm_get() != NULL);
	note_fails = true;
	SIFT_CHECKF(write_raises(REL + 200), "the note of the evictions fails");
	for (int i = 0; i < LSHM_FLIGHTS; i++)
		SIFT_CHECKF(lshm->flight[i].len == 0, "a writer that failed holds flight %d of the ring for ever", i);
	lshm->head = 0;
	SIFT_CHECK(!write_raises(REL + 201));
	entries(REL + 201, &n, &stamp);
	SIFT_CHECKF(n == 1, "the place a failed writer copied into is taken by the next writer (%d entries)", n);
	free(lshm);
	lshm = NULL;
}

int
main(void)
{
	test_overtaken_writer();
	test_wrap_onto_copy();
#ifndef __SANITIZE_THREAD__
	/* one thread; ThreadSanitizer does not find the jmp_buf the ERROR goes to */
	test_failed_note_frees_flight();
#endif
	return sift_check_summary();
}
