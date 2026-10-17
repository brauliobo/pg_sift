/*
 * sift_prefix.h - postings of a prefix with more terms than sift.max_expansions.
 */
#ifndef SIFT_PREFIX_H
#define SIFT_PREFIX_H

#include "sift_segment.h"

/*
 * The term a wide leaf reads as in one segment: the documents that have a word matched by m, tf the
 * occurrences of those words (what the indexed prefix term of prefix_index holds). Returns the term blob
 * (see sift_postw_end_term_mem; NULL for an inline term or no document) and fills info; info->df is 0 when
 * no live or dead document has such a word. key names the leaf (kind, field and text) for the cache, NULL
 * when it is not to be cached. With scoring false, tf and bounds are not computed
 * (every tf is 1), which is all a match or a count needs. Allocates the blob in mcxt.
 */
extern const uint8 *sift_prefix_union(SiftSegment *seg, const SiftTermMatcher *m, const char *key, int keylen,
									  const SiftScoreRef *ref, bool scoring, MemoryContext mcxt, SiftTermInfo *info);

/* sift.prefix_cache_size (kB): the blobs this backend keeps for the next bind, 0 = none. key names the term (indexed_leaf's) */
extern PGDLLIMPORT int sift_prefix_cache_size;

/* kB of postings one pass of a wide prefix gathers: past it the superblocks are built in windows that fit (a test GUC) */
extern PGDLLIMPORT int sift_prefix_gather_kb;

#ifdef SIFT_TESTING
/* What the passes of this backend did (sift._test_prefix_stats) */
typedef struct SiftPrefixStats
{
	uint64		passes;			/* scans of the expansion's terms */
	uint64		windows;		/* passes over part of the segment */
	uint64		direct;			/* ... of one superblock, summed on the spot */
	uint64		peak;			/* most postings one pass gathered */
} SiftPrefixStats;
extern SiftPrefixStats sift_prefix_stats;
#endif

#endif							/* SIFT_PREFIX_H */
