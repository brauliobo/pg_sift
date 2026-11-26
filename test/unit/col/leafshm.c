/*
 * test/unit/col/leafshm.c - the writers of the shared statistics of col/leafshm.c that come between the two halves of a write (the place in the ring taken, the directory not yet entered): the
 * directory keeps one entry for an index and kind, the one of the newest stamp, and the entry of another index that took the place the first writer chose is not thrown out for it.
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

enum { REL = 16384 };

static RelFileNumber other_rel;
static bool between;

Size
sift_shm_pool_bytes(SiftShmPoolId pool)
{
	return 1 << 20;
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

/* Between its two halves, the first writer is overtaken: another index of its bucket enters the directory, then a newer record of the same index */
void
sift_shm_note_eviction(SiftShmPoolId pool, uint64 count)
{
	if (!between)
		return;
	between = false;
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
	between = true;
	pre_write(REL, 1);
	entries(REL, &n, &stamp);
	SIFT_CHECKF(n == 1 && stamp == 2, "the index has %d entries of its kind, the last of stamp %llu: one of stamp 2 is wanted", n, (unsigned long long) stamp);
	entries(other_rel, &n, &stamp);
	SIFT_CHECKF(n == 1, "the other index of the bucket has %d entries", n);
}

int
main(void)
{
	test_overtaken_writer();
	return sift_check_summary();
}
