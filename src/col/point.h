/*
 * col/point.h - the point directory of a leaf in a segment: an open addressing table of the 32-bit
 * fingerprints and row numbers of every row of the segment that holds a value (the key of the row, the code of a dictionary leaf), so that an equality
 * or a short IN list finds its rows (and the units that hold them) with a probe or two, where the zones and small-set summaries only cut the units
 * that cannot hold the value. The table does not say that a row holds the value, only that its fingerprint matches: the scans that use it (the access
 * method of sift_col) have the heap check the condition again. Portable (core/host.h only).
 */
#ifndef SIFT_COL_POINT_H
#define SIFT_COL_POINT_H

#include "col/enc.h"
#include "core/host.h"

#define SIFT_CPOINT_MAGIC	0x4B4C4F43		/* "COLK" */
#define SIFT_CPOINT_VERSION	1
#define SIFT_CPOINT_EMPTY	UINT64_MAX
#define SIFT_CPOINT_MAX_KEYS	64			/* an IN list or a range of more keys is answered by the units */
#define SIFT_CPOINT_MAX_ROWS	65536		/* a lookup that finds more rows is not a point lookup: the units answer it */
#define SIFT_CPOINT_MAX_RUN		1024		/* a leaf whose rows crowd one position (one key in about this many rows) gets no table */

/* A table is for a plain integer-like, text or uuid column of the top level (the leaves a small-set summary takes) */
extern const char *sift_cpoint_spec_parse(const char *opt, const SiftColSchema *s, const char *const *names, bool *leaf);
extern const char *sift_cpoint_syntax(const char *opt);

extern uint64 sift_cpoint_hash(int64 key);

/* The slots of a table of n entries: a power of two, at most 3/4 full */
extern uint32 sift_cpoint_slots(uint32 n);

/* A slot is the fingerprint (the low 32 bits of the hash) over the row number; it starts at the position the high 32 bits give */
static inline uint32
sift_cpoint_pos(uint64 hash, uint32 nslots)
{
	return (uint32) (hash >> 32) & (nslots - 1);
}

static inline bool
sift_cpoint_slot_is(uint64 slot, uint64 hash)
{
	return (slot >> 32) == (hash & 0xFFFFFFFF);
}

/* Appends the table (nslots * 8 bytes) of the n entries (key, row) to out; false (nothing appended): too many rows crowd one position */
extern bool sift_cpoint_build(const int64 *key, const uint32 *row, uint32 n, uint32 nslots, StringInfo out);

#endif
