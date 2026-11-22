/*
 * core/f2/fsm/release_guard.c - what a release of extents must hold before it is queued (core/f2/fsm.h): the extents are blocks of the file past the metapage and the root of the allocator, and no
 * block is named twice. A caller that releases a block it does not own is caught here, at the call that did it; later the queue can only say that two extents overlap (reclaim), not who put them.
 * A testing or assert build also checks the block against what is free or queued already.
 */
#include "core/host.h"

#include "core/f2/fsm.h"
#include "core/f2/fsm/fsm_int.h"
#include "core/f2/meta.h"

static int
ext_start_cmp(const void *a, const void *b)
{
	BlockNumber x = ((const SiftExtent *) a)->start,
				y = ((const SiftExtent *) b)->start;

	return x < y ? -1 : x > y;
}

static void
check_batch(Relation rel, const SiftExtent *ext, int n)
{
	SiftExtent *sorted = n > 1 ? palloc(n * sizeof(SiftExtent)) : NULL;

	for (int i = 0; i < n; i++)
		if (ext[i].nblocks == 0 || ext[i].start <= SIFT_FSM_BLKNO || (uint64) ext[i].start + ext[i].nblocks > SIFT_F2_LOCK_FLOOR)
			SIFT_CORRUPT(rel, "A release of %u blocks at block %u is not a range of blocks that can be freed.", ext[i].nblocks, ext[i].start);
	if (n > 1)
	{
		memcpy(sorted, ext, n * sizeof(SiftExtent));
		qsort(sorted, n, sizeof(SiftExtent), ext_start_cmp);
		for (int i = 1; i < n; i++)
			if ((uint64) sorted[i - 1].start + sorted[i - 1].nblocks > sorted[i].start)
				SIFT_CORRUPT(rel, "A release names block %u twice.", sorted[i].start);
		pfree(sorted);
	}
}

#if defined(SIFT_TESTING) || defined(USE_ASSERT_CHECKING)
/* Is a block of the release in a list already: the double free, found at the caller that makes it */
static void
check_unheld(Relation rel, Page root, const SiftExtent *ext, int n)
{
	SiftFsmMetaPageData *m = FSMM(root);
	ExtVec		held = {0},
				pages = {0};

	for (int k = 0; k < SIFT_FSM_NCLASSES; k++)
		sift_fsm_walk_list(rel, m->class_head[k], k, &held, &pages);
	sift_fsm_walk_list(rel, m->quarantine_head, SIFT_FSM_CLASS_QUEUE, &held, &pages);
	for (int i = 0; i < n; i++)
		for (int j = 0; j < held.n; j++)
			if ((uint64) ext[i].start < (uint64) held.v[j].start + held.v[j].nblocks && (uint64) held.v[j].start < (uint64) ext[i].start + ext[i].nblocks)
				SIFT_CORRUPT(rel, "A release of block %u frees a block that is free or queued already (duplicate release of block %u).", ext[i].start, held.v[j].start);
}
#endif

void
sift_fsm_release_guard(Relation rel, Page root, const SiftExtent *ext, int n)
{
	check_batch(rel, ext, n);
#if defined(SIFT_TESTING) || defined(USE_ASSERT_CHECKING)
	check_unheld(rel, root, ext, n);
#endif
}
