/*
 * sift_merge.h - which segments to merge: the tiered policy shared by the hosts (DESIGN.md (c), "Shared core").
 * How a merge is carried out (runs in PostgreSQL, a lockstep stream in SQLite) and how the result replaces its inputs in the
 * directory are the host's.
 */
#ifndef SIFT_MERGE_H
#define SIFT_MERGE_H

#include "sift_host.h"

#include "sift_segment.h"

#define SIFT_MERGE_FANIN		8		/* segments of a tier that trigger a merge (the default of a host that lets it be set) */
#define SIFT_MERGE_MAX_SEGMENTS	32		/* more directory entries than this trigger a merge of the smallest segments */

typedef struct SiftMergePolicy
{
	uint64		tier_base;		/* bytes: segments below tier_base * fanin are tier 0, then every fanin-fold of size is a tier */
	int			fanin;
	bool		(*mostly_dead) (Relation index, const SiftSegEntry *e);	/* NULL: the BM25 segment reader's test */
} SiftMergePolicy;

/*
 * The immutable segments of snap to merge now, in sel (room for SIFT_MAX_SEGMENTS), and their number: fanin or more of one tier
 * (at most 2 * fanin of it), else the smallest fanin when the directory has more than SIFT_MERGE_MAX_SEGMENTS entries, else a
 * lone segment that is more than half dead. force: all of them, a lone one only if some of its documents are dead (the rewrite
 * drops them and their terms). 0 when there is nothing to merge.
 */
extern int	sift_merge_pick(Relation index, const SiftMergePolicy *policy, const SiftDirSnapshot *snap, bool force,
							SiftSegEntry *sel);

/* Only the last of those: the first segment (smallest first) of which more than half the documents are dead, else 0. */
extern int	sift_merge_pick_dead(Relation index, const SiftDirSnapshot *snap, SiftSegEntry *sel);

#endif							/* SIFT_MERGE_H */
