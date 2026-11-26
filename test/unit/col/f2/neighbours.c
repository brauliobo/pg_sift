/*
 * test/unit/col/f2/neighbours.c - the merge of small neighbours (col/f2/compact/neighbours.c) under a writer: a writer that puts rows in a partition between the look at the pair and the
 * locks of the merge (it holds them while it commits) leaves the pair alone, and the VACUUM that runs the merge goes on, not stopped by the error of a merge of a partition that holds rows.
 */
#include "postgres.h"

#include "col/f2/compact/neighbours.c"

#include "test/common/check.h"
#include "test/common/pgshim.h"

sigjmp_buf *PG_exception_stack;
ErrorContextCallback *error_context_stack;

enum { NPARTS = 3 };

static SiftPartitionEntry manifest[NPARTS];	/* the manifest as it is */
static int	writer_at = -1;			/* the partition a writer commits rows to when the merge next takes the locks of a pair */
static int	merges;

void
pg_re_throw(void)
{
	siglongjmp(*PG_exception_stack, 1);
}

static void
writer_commits(void)
{
	if (writer_at >= 0)
		manifest[writer_at].l0_slab_blk = 100;
	writer_at = -1;
}

bool
ConditionalLockPage(Relation relation, BlockNumber blkno, LOCKMODE lockmode)
{
	return true;
}

void
UnlockPage(Relation relation, BlockNumber blkno, LOCKMODE lockmode)
{
}

#if PG_VERSION_NUM >= 180000
void
vacuum_delay_point(bool is_analyze)
{
}
#else
void
vacuum_delay_point(void)
{
}
#endif

void
sift_manifest_lock_pair(Relation rel, uint64 a, uint64 b, bool acquire)
{
	if (acquire)
		writer_commits();
}

SiftManifestRead
sift_manifest_read_range(Relation rel, const uint8 *lo, const uint8 *hi)
{
	SiftPartitionEntry *copy = palloc(sizeof(manifest));

	memcpy(copy, manifest, sizeof(manifest));
	return (SiftManifestRead) {.entries = copy,.n = NPARTS};
}

bool
sift_manifest_find_entry(Relation rel, uint64 part_id, const uint8 *key_lo, SiftPartitionEntry *out)
{
	for (int i = 0; i < NPARTS; i++)
		if (manifest[i].part_id == part_id)
		{
			*out = manifest[i];
			return true;
		}
	return false;
}

/* What build_merge does under the locks of the pair: a partition that holds rows is refused with an error */
void
sift_manifest_merge(Relation rel, uint64 keep_id, const uint8 *keep_lo, uint64 drop_id, const uint8 *drop_lo)
{
	writer_commits();
	for (int i = 0; i < NPARTS; i++)
		if ((manifest[i].part_id == keep_id || manifest[i].part_id == drop_id) && manifest[i].l0_slab_blk != InvalidBlockNumber)
			pg_re_throw();
	merges++;
}

static void
fresh(void)
{
	for (int i = 0; i < NPARTS; i++)
		manifest[i] = (SiftPartitionEntry) {.part_id = i + 1,.l0_slab_blk = InvalidBlockNumber,.pend_del_blk = InvalidBlockNumber,.total_bytes = 10};
	merges = 0;
}

/* The writer commits to the second partition as the merge locks the first pair: that pair is left, the second pair (2, 3) is judged as it is now, and VACUUM goes on */
static void
test_writer_between_look_and_lock(void)
{
	volatile bool failed = false;
	volatile int merged = 0;

	fresh();
	writer_at = 1;
	PG_TRY();
	{
		merged = sift_compact_merge_neighbours(NULL);
	}
	PG_CATCH();
	{
		failed = true;
	}
	PG_END_TRY();
	SIFT_CHECKF(!failed, "a writer between the look at a pair and its locks stops the VACUUM with an error");
	SIFT_CHECKF(merged == 0 && merges == 0, "a pair whose partition took rows is merged: %d merged", merged);
}

static void
test_idle_pairs_merge(void)
{
	fresh();
	SIFT_CHECKF(sift_compact_merge_neighbours(NULL) == 2 && merges == 2, "three small idle partitions become one: %d merges", merges);
}

int
main(void)
{
	pgshim_keep();
	test_idle_pairs_merge();
	test_writer_between_look_and_lock();
	return sift_check_summary();
}
