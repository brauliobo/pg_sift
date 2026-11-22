/*
 * agg/group/dist.h - the set of the distinct values of a count(DISTINCT), exact for every value. A set holds one of two kinds of value, the kind of its first add:
 *   - uint64 values, for a value that is its own 64-bit image (an integer, a date or a timestamp, a dictionary code of one dictionary): an open-addressing table
 *     of the values, with 0 kept apart so that an empty cell needs no marker word. The value is never a hash: two values that share 64 bits would be one;
 *   - bytes, for a value that is wider (text, numeric, bytea, uuid): the bytes of each distinct value kept in an arena next to the hash they were added with, the
 *     table finds a value by that hash and says it is a repeat only when the length and every byte are equal. A hash only chooses where to look.
 * A set is merged into another by adding its values, which is how the partitions of a parallel merge give the exact distinct count of a group that several workers
 * saw. The memory of a set is sift_dist_bytes: the arena grows with the distinct bytes, so the caller that holds work_mem for its sets counts it.
 */
#ifndef SIFT_AGG_GROUP_DIST_H
#define SIFT_AGG_GROUP_DIST_H

#include "core/host.h"

typedef struct SiftDistSet
{
	uint64	   *cells;			/* cap cells, 0 = empty; a value, or the offset + 1 of an entry of the arena */
	uint32		cap;			/* a power of 2 */
	uint32		used;			/* the nonzero values, or the entries of the arena */
	bool		has_zero;
	bool		wide;			/* the values are bytes */
	uint8	   *arena;			/* wide: entries of {hash, length, bytes}, each at an 8 byte boundary */
	uint64		arena_used;
	uint64		arena_cap;
	MemoryContext cxt;
} SiftDistSet;

/* An empty set in the current memory context */
extern SiftDistSet *sift_dist_new(void);
extern void sift_dist_free(SiftDistSet *s);
/* The bytes the set holds: the header, the cells and the arena */
extern uint64 sift_dist_bytes(const SiftDistSet *s);
/* True when v was not in the set. v must be the value itself, never the hash of a wider value: use sift_dist_add_bytes for those */
extern bool sift_dist_add(SiftDistSet *s, uint64 v);
/* True when the len bytes were not in the set */
extern bool sift_dist_add_bytes(SiftDistSet *s, const void *bytes, uint32 len);
/* The same with the hash of the bytes given (sift_hash_bytes of them, or whatever the caller keeps equal for equal bytes: the set compares the bytes) */
extern bool sift_dist_add_hashed(SiftDistSet *s, uint64 hash, const void *bytes, uint32 len);
extern uint64 sift_dist_count(const SiftDistSet *s);
extern void sift_dist_merge(SiftDistSet *dst, const SiftDistSet *src);
/* The next value at or after *pos (the cell index, 0 to start); false at the end. Every value is returned once, in no order */
extern bool sift_dist_next(const SiftDistSet *s, uint32 *pos, uint64 *v);
/* The same for a set of bytes: the bytes stay valid until the set is added to */
extern bool sift_dist_next_bytes(const SiftDistSet *s, uint32 *pos, const uint8 **bytes, uint32 *len);

#endif							/* SIFT_AGG_GROUP_DIST_H */
