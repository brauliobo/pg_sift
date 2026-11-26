/*
 * col/f2/build/par.h - what the participants of a parallel build of format major 2 leave for the leader: the parts they wrote wait in shared memory, and the leader takes all of them
 * into the manifest in one commit once the workers are done; the generations of the parts come from a range the leader reserved, so that a run does not lock the metapage for its own.
 */
#ifndef SIFT_F2_BUILD_PAR_H
#define SIFT_F2_BUILD_PAR_H

#include "port/atomics.h"

#include "col/f2/build/build.h"
#include "col/f2/part/format.h"

/* The generations a participant is expected to write its runs with: a participant that writes more asks the manifest for the rest one at a time */
#define SIFT_F2_BUILD_GENS_PER_PARTICIPANT	4

typedef struct SiftF2BuildGens
{
	pg_atomic_uint64 next;
	uint64		end;
} SiftF2BuildGens;

/* The parts of one participant that no commit has taken yet, and whether any of them is small enough to be packed (sift_compact_pack_tails) */
typedef struct SiftF2BuildHand
{
	uint32		n;
	bool		tiny;
	SiftPartDesc parts[SIFT_PARTLIST_CAP];
} SiftF2BuildHand;

/* The participant leaves the parts it ends with in hand, and takes the generations of its runs from gens, instead of committing and asking the manifest */
extern void sift_f2_build_share(SiftF2Build *bs, SiftF2BuildHand *hand, SiftF2BuildGens *gens);

/* The leader: the parts of the hands[0 .. n) and its own are committed together; whether any of them is tiny is the answer of sift_f2_build_tiny */
extern void sift_f2_build_commit_hands(SiftF2Build *leader, const SiftF2BuildHand *hands, int n);
extern bool sift_f2_build_tiny(const SiftF2Build *bs);

#endif							/* SIFT_F2_BUILD_PAR_H */
