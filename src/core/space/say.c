/*
 * core/space/say.c - the INFO lines of the cut and the shrink of the free end of an index file (VACUUM VERBOSE, the log of an autovacuum): what they report and the bytes of it, in one place;
 * the cut itself is in space.c
 */
#include "postgres.h"

#include "core/format_ux.h"
#include "core/maint/maint.h"
#include "core/maint/int.h"

static const char *
blocks_size(char *buf, int64 blocks)
{
	return sift_fmt_bytes(buf, SIFT_UX_QUANTITY, (uint64) blocks * BLCKSZ);
}

/* The free end of the file was given back: the pages before and after, and the bytes returned */
void
sift_cut_say_truncated(Relation rel, const char *what, int elevel, BlockNumber freed, bool online)
{
	char		size[SIFT_UX_QUANTITY];

	ereport(elevel, (errmsg("%s \"%s\": truncated %u to %u pages (reclaimed %s%s)", what, RelationGetRelationName(rel), RelationGetNumberOfBlocks(rel) + freed,
							RelationGetNumberOfBlocks(rel), blocks_size(size, freed), online ? ", online" : "")));
}

/* Segments at the end of the file were moved into free extents below them so that the file can be cut */
void
sift_cut_say_relocated(Relation rel, int elevel, uint64 bytes)
{
	char		size[SIFT_UX_QUANTITY];

	ereport(elevel, (errmsg("index \"%s\": relocated %s from the end of the file into lower free extents", RelationGetRelationName(rel), sift_fmt_bytes(size, sizeof(size), bytes))));
}

/* why: what kept the cut; blocks: the free blocks that stay in the file (SKIP_FALLBACK: the blocks moved); waited: the ms the lock was asked for; more: the blocks given back (SKIP_FALLBACK) */
SiftShrinkBlock
sift_cut_say_skipped(Relation rel, const char *what, int elevel, CutSkip why, int64 blocks, int waited, int64 more)
{
	const char *name = RelationGetRelationName(rel);
	char		size[SIFT_UX_QUANTITY];

	switch (why)
	{
		case SKIP_STANDBY:
			ereport(elevel, (errmsg("%s \"%s\": not truncating while a standby streams: %lld free blocks (%s) stay", what, name, (long long) blocks, blocks_size(size, blocks))));
			return SIFT_SHRINK_STANDBY;
		case SKIP_LOCK:
			ereport(elevel, (errmsg("%s \"%s\": not truncating, a lock is held on it (waited %d ms): %lld free blocks (%s) stay", what, name, waited, (long long) blocks, blocks_size(size, blocks))));
			return SIFT_SHRINK_LOCK;
		case SKIP_FALLBACK:
			ereport(elevel, (errmsg("%s \"%s\": a lock is held on it (waited %d ms): %lld blocks moved, %lld given back online (%s)", what, name, waited, (long long) blocks, (long long) more, blocks_size(size, more))));
			return SIFT_SHRINK_OPEN;
		case SKIP_SNAPSHOT:
			ereport(elevel, (errmsg("%s \"%s\": %lld free blocks (%s) stay, held by a snapshot that may still read them", what, name, (long long) blocks, blocks_size(size, blocks))));
			return SIFT_SHRINK_SNAPSHOT;
	}
	return SIFT_SHRINK_LOCK;
}
