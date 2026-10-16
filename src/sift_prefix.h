/*
 * sift_prefix.h - postings of a prefix with more terms than sift.max_expansions (DESIGN.md (h)).
 */
#ifndef SIFT_PREFIX_H
#define SIFT_PREFIX_H

#include "sift_segment.h"

/*
 * The term a wide prefix reads as in one segment: the documents that have a word matched by m, tf the
 * occurrences of those words (what the indexed prefix term of prefix_index holds). Returns the term blob
 * (see sift_postw_end_term_mem; NULL for an inline term or no document) and fills info; info->df is 0 when
 * no live or dead document has such a word. key names the term (the wide leaf's, field byte included) for the cache. With scoring false, tf and bounds are not computed
 * (every tf is 1), which is all a match or a count needs. Allocates the blob in mcxt.
 */
extern const uint8 *sift_prefix_union(SiftSegment *seg, const SiftTermMatcher *m, const char *key, int keylen,
									  const SiftScoreRef *ref, bool scoring, MemoryContext mcxt, SiftTermInfo *info);

/* sift.prefix_cache_size (kB): the blobs this backend keeps for the next bind, 0 = none. key names the term (indexed_leaf's) */
extern PGDLLIMPORT int sift_prefix_cache_size;

#endif							/* SIFT_PREFIX_H */
