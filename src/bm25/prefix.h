/*
 * bm25/prefix.h - postings of a prefix with more terms than sift.max_expansions.
 */
#ifndef SIFT_PREFIX_H
#define SIFT_PREFIX_H

#include "bm25/segment/segment.h"

/*
 * The term a wide leaf reads as in one segment: the documents that have a word matched by m, tf the most
 * occurrences of one of those words. Returns the term blob
 * (see sift_postw_end_term_mem; NULL for an inline term or no document) and fills info; info->df is 0 when
 * no live or dead document has such a word. key names the leaf (kind, field and text) for the cache, NULL
 * when it is not to be cached. With scoring false, tf and bounds are not computed
 * (every tf is 1), which is all a match or a count needs. Allocates the blob in mcxt.
 * The terms with a bound below min_bound are left out (such a union is not cached); left_out is the largest bound of those, when it is not NULL.
 */
extern const uint8 *sift_prefix_union(SiftSegment *seg, SiftTermMatcher *m, const char *key, int keylen,
									  const SiftScoreRef *ref, bool scoring, int min_bound, MemoryContext mcxt, SiftTermInfo *info,
									  uint8 *left_out);
/* The index file is built again in place: the unions kept of its segments name the contents before */
extern void sift_prefix_drop_file(const RelFileLocator *loc);

/* kB of postings one pass of a wide prefix gathers: past it the superblocks are built in windows that fit (a test GUC) */
extern PGDLLIMPORT int sift_prefix_gather_kb;

#ifdef SIFT_TESTING
/* What the passes of this backend did (sift._test_prefix_stats) */
typedef struct SiftPrefixStats
{
	uint64		passes;			/* scans of the expansion's terms */
	uint64		dict_scans;		/* ... that read the dictionary (the first of a segment: the windows read the terms it kept) */
	uint64		windows;		/* passes over part of the segment */
	uint64		direct;			/* ... of one superblock, summed on the spot */
	uint64		peak;			/* most postings one pass gathered */
} SiftPrefixStats;
extern SiftPrefixStats sift_prefix_stats;
#endif

#endif							/* SIFT_PREFIX_H */
