/*
 * core/space/move.c - the relocation step of the space step (core/space/space.c): a segment written again into the free blocks below it, and what the kind of the index does in that switch
 */
#include "postgres.h"
#include "utils/rel.h"
#include "utils/timestamp.h"
#include "core/maint/maint.h"
#include "core/maint/int.h"
#include "core/ops.h"
#include "core/store/store.h"

void
sift_space_move(Relation rel, SiftWriteTarget *wt, const void *arg)
{
	SpaceMove  *move = (SpaceMove *) arg;
	const SiftMaintOps *ops = sift_mops_of(rel);
	SiftSegEntry fresh;
	SiftMaintSwitch sw = {.old = &move->entry->gen,.nold = 1,.neu = &fresh};

	move->written = sift_store_relocate(rel, wt, move->entry, move->release, move->nrelease,
									 move->limit, move->budget, &fresh, move->relocate_page);
	if (move->written == 0)
		return;
	move->header = fresh.header_blk;
	if (ops && ops->relocated)
		ops->relocated(rel, move->entry->gen, fresh.gen, move->recut);
	sift_wt_publish(wt, &(SiftDirChange) {.fn = sift_maint_switch_segments,.arg = &sw,
					.release = move->release,.nrelease = move->nrelease,.extra = InvalidBuffer});
	if (ops && ops->switched)
		ops->switched(rel);
	sift_log_maint(rel, "relocated", GetCurrentTimestamp(), move->entry->ndocs, fresh.bytes);
}

#ifdef SIFT_TESTING
/*
 * Moves the segment that ends the file highest into the free blocks below, whatever the screens of the space step say (the caller holds the relation exclusively): the switch
 * of a split of a partition that copies units, for the tests of the deletion records. recut is a mask of the units the move cuts again.
 */
bool
sift_maint_relocate_test(Relation rel, uint64 recut)
{
	SiftDirSnapshot *snap = sift_dir_read(rel);
	SpaceMove	move = {0};
	SiftExtent *release = NULL;
	BlockNumber top = 0;
	int			nrelease = 0;

	for (int i = 0; i < snap->nsegs; i++)
	{
		int			k;
		SiftExtent *ext;

		if (snap->segs[i].kind != SIFT_SEG_IMMUTABLE)
			continue;
		ext = sift_store_immutable_extents(rel, &snap->segs[i], &k);
		for (int x = 0; x < k; x++)
			if (ext[x].start + ext[x].nblocks > top)
			{
				top = ext[x].start + ext[x].nblocks;
				move.entry = &snap->segs[i];
				release = ext;
				nrelease = k;
			}
	}
	if (move.entry == NULL)
		return false;
	move.release = release;
	move.nrelease = nrelease;
	move.limit = snap->meta.high_water;
	move.budget = PG_UINT64_MAX;
	move.recut = recut;
	sift_store_space_exclusive(rel, true);
	PG_TRY();
	{
		(void) sift_recycle(rel);
		run_step(rel, false, sift_space_move, &move);
	}
	PG_FINALLY();
	{
		sift_store_space_exclusive(rel, false);
	}
	PG_END_TRY();
	return move.written > 0;
}
#endif
