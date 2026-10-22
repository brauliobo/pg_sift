/*
 * col/point.h - the point directory of a leaf in a segment: an open addressing table with a slot for each
 * distinct key of the leaf (the key of a row, the code of a dictionary leaf), holding the 32-bit fingerprint of the key and either its only row or where the
 * list of its rows is, so that an equality or a short IN list finds its rows (and the units that hold them) with a probe or two, where the zones and
 * small-set summaries only cut the units that cannot hold the value. The table does not say that a row holds the value, only that its fingerprint matches:
 * the scans that use it (the access method of sift_col) have the heap check the condition again. Portable (core/host.h only).
 */
#ifndef SIFT_COL_POINT_H
#define SIFT_COL_POINT_H

#include "col/enc.h"
#include "core/host.h"

#define SIFT_CPOINT_MAGIC	0x4B4C4F43		/* "COLK" */
#define SIFT_CPOINT_VERSION	2
#define SIFT_CPOINT_EMPTY	UINT64_MAX
#define SIFT_CPOINT_MAX_KEYS	64			/* an IN list or a range of more keys is answered by the units */
#define SIFT_CPOINT_MAX_ROWS	65536		/* a lookup that finds more rows is not a point lookup: the units answer it */
#define SIFT_CPOINT_SEG_ROWS	(UINT32_C(1) << 30)	/* a segment of this many rows or more has no tables: a slot keeps a list offset in 31 bits */
#define SIFT_CPOINT_LIST		(UINT32_C(1) << 31)	/* in a slot, the low 32 bits are this flag and the word offset of a list of rows, not the only row of the key */
#define SIFT_CPOINT_RECLEN		32			/* the bytes of a record of the header */

/* A table is for a plain integer-like, text or uuid column of the top level (the leaves a small-set summary takes) */
extern const char *sift_cpoint_spec_parse(const char *opt, const SiftColSchema *s, const char *const *names, bool *leaf);
extern const char *sift_cpoint_syntax(const char *opt);

extern uint64 sift_cpoint_hash(int64 key);

/* The slots of a table of n keys: a power of two, at most 3/4 full */
extern uint32 sift_cpoint_slots(uint32 n);

/* A slot is the fingerprint (the low 32 bits of the hash) over the row of a key with one row, or SIFT_CPOINT_LIST and the word offset of its list; it starts at the position the high 32 bits give */
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

/*
 * Appends the leaf's block for the n entries (key, row) to out, rows ascending in each key: the table (nslots * 8 bytes), then the lists, each {u32 count (2 or
 * more), u32 rows[count]} (words * 4 bytes, padded to 8), for the keys with more than one row. nkeys is the distinct keys.
 */
extern void sift_cpoint_build(const int64 *key, const uint32 *row, uint32 n, StringInfo out, uint32 *nslots, uint32 *nkeys, uint32 *words);

#endif
