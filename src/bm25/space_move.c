/*
 * bm25/space_move.c - the relocation step of the space step (bm25/space.c): a segment written again into the free blocks below it, and what the deletion records of a column index do in that switch
 */
#include "postgres.h"
#include "port/pg_bitutils.h"
#include "utils/rel.h"
#include "utils/timestamp.h"
#include "bm25/maint.h"
#include "bm25/maint_int.h"
#include "col/pg.h"
#include "col/wbuf.h"
#include "core/ops.h"
#include "core/store.h"

/*
 * A relocation copies the pages of a segment as they are, so every unit of the new generation is the unit of the old one with the same number: the deletion records of the segment are
 * written again under the new generation (sift_col_dv_rekey). A testing build can name units that the move treats as cut again, whose records are carried to marks instead.
 */
#ifdef SIFT_TESTING
static uint64 space_recut_units = 0;
#define SPACE_RECUT_UNITS	space_recut_units
#else
#define SPACE_RECUT_UNITS	UINT64CONST(0)
#endif

static int
space_dv_moves(uint64 gen, uint64 gen2, SiftDvMove *moves)
{
	uint32		start = 0;
	int			n = 0;

	for (uint64 cut = SPACE_RECUT_UNITS; cut != 0; cut &= cut - 1)
	{
		uint32		unit = pg_rightmost_one_pos64(cut);

		if (unit > start)
			moves[n++] = (SiftDvMove) {gen, start, unit - start, gen2, start};
		start = unit + 1;
	}
	moves[n++] = (SiftDvMove) {gen, start, PG_UINT32_MAX - start, gen2, start};
	return n;
}

void
sift_space_move(Relation rel, SiftWriteTarget *wt, const void *arg)
{
	SpaceMove  *move = (SpaceMove *) arg;
	SiftSegEntry fresh;
	SiftDvMove	dv[65];
	SiftMaintSwitch sw = {.old = &move->entry->gen,.nold = 1,.neu = &fresh};

	move->written = sift_store_relocate(rel, wt, move->entry, move->release, move->nrelease,
									 move->limit, move->budget, &fresh, move->relocate_page);
	if (move->written == 0)
		return;
	move->header = fresh.header_blk;
	if (sift_col_is_index(rel))
		sift_col_dv_rekey(rel, &move->entry->gen, 1, dv, space_dv_moves(move->entry->gen, fresh.gen, dv));
	sift_wt_publish(wt, &(SiftDirChange) {.fn = sift_maint_switch_segments,.arg = &sw,
					.release = move->release,.nrelease = move->nrelease,.extra = InvalidBuffer});
	if (sift_col_is_index(rel))
		sift_col_dv_switched(rel);
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
	space_recut_units = recut;
	sift_store_space_exclusive(rel, true);
	PG_TRY();
	{
		(void) sift_recycle(rel);
		run_step(rel, false, sift_space_move, &move);
	}
	PG_FINALLY();
	{
		sift_store_space_exclusive(rel, false);
		space_recut_units = 0;
	}
	PG_END_TRY();
	return move.written > 0;
}
#endif
