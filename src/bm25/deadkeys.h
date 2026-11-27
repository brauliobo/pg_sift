/*
 * bm25/deadkeys.h - the docs of immutable segments that a heap check found dead to every snapshot, kept by the backend for its next searches.
 */
#ifndef SIFT_BM25_DEADKEYS_H
#define SIFT_BM25_DEADKEYS_H

#include "bm25/segment/segment.h"

/*
 * A delete leaves the doc in its segment, live, until a VACUUM of the index clears it, and a ranked search verifies in the heap every doc that outscores its K-th live hit:
 * with the best docs deleted it reads their heap pages again at every statement. A doc whose tuple is dead to every snapshot stays dead (its TID is not reused before the
 * VACUUM that clears the doc, and a segment never takes a doc back), so the search that finds it dead remembers it under the segment (the index file and the generation
 * name it for good), and the next ones leave it out of the docs they score.
 */
extern void sift_deadkeys_note(const SiftSegment *seg, SiftKey key);

/* Clears from res (the offsets of the SIFT_BLK_PAGES pages of block b of superblock sb) the docs the backend knows dead */
extern void sift_deadkeys_clear(const SiftSegment *seg, uint32 sb, int b, uint64 (*res)[SIFT_OFFMASK_WORDS]);

/* The index file is built again in place: the docs known dead in its segments were of the contents before */
extern void sift_deadkeys_drop_file(const RelFileLocator *loc);

#endif							/* SIFT_BM25_DEADKEYS_H */
