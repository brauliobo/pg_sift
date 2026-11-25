/*
 * core/shrink.c - what the cut of the file of an index left behind, kept in its metapage (sift.col_shrink_history): why the free blocks stay in the file, and when the file was last cut
 */
#include "postgres.h"

#include "access/xlog.h"
#include "utils/rel.h"
#include "utils/timestamp.h"

#include "core/maint/maint.h"

const char *
sift_shrink_block_name(SiftShrinkBlock why)
{
	static const char *const names[] = {"none", "standby", "lock", "snapshot"};

	return names[why];
}

typedef struct ShrinkNote
{
	SiftShrinkBlock why;
	TimestampTz shrunk;			/* 0: the file was not cut */
} ShrinkNote;

static bool
stamp_shrink(SiftMetaPageData *meta, void *arg)
{
	const ShrinkNote *n = arg;

	if (meta->shrink_blocked == n->why && n->shrunk == 0)
		return false;
	meta->shrink_blocked = n->why;
	if (n->shrunk != 0)
		meta->last_shrink = n->shrunk;
	return true;
}

void
sift_maint_note_shrink(Relation rel, SiftShrinkBlock why, bool shrunk)
{
	ShrinkNote	n = {why, shrunk ? GetCurrentTimestamp() : 0};

	if (rel->rd_rel->relkind != RELKIND_INDEX || RecoveryInProgress())
		return;
	/* a cut with no reason of its own that leaves blocks free but not reusable yet: a snapshot that may still read them holds them */
	if (why == SIFT_SHRINK_OPEN)
	{
		SiftFreeStats fs = sift_store_free_stats(rel);

		if (sift_maint_space_worth(fs.blocks - fs.reusable, RelationGetNumberOfBlocks(rel)))
			n.why = SIFT_SHRINK_SNAPSHOT;
	}
	sift_meta_update(rel, stamp_shrink, &n);
}
