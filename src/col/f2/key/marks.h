/*
 * col/f2/key/marks.h - SEC_KEY_MARKS (0x020D), the sparse primary index of a part: for every granule the key tuple (1 to 8 columns, col/f2/key/enc.h) of its first and of its last row. A part is
 * sorted by (key, _tid), so the marks ascend over the granules and the granules that can hold the rows a set of predicates allows are found by binary search over bytes in memory, whatever the
 * number of granules, with no buffer read for a probe (col/f2/key/plan.h). The section is read once for a part and kept.
 *
 * A mark is the first key_bytes of the encoding of the tuple, zero padded: a tuple that takes more is cut, which keeps the order (a cut tuple is not above the whole one) and loses only the
 * columns past the cut. SIFT_KEY_MARKS_EXACT in the flags says that no tuple was cut, which lets a scan tell a granule that starts at the end of a range from one that starts inside it.
 */
#ifndef SIFT_KEY_MARKS_H
#define SIFT_KEY_MARKS_H

#include "core/host.h"

#include "col/f2/key/enc.h"

#define SIFT_KEY_MARKS_MAGIC	0x4B4D524BU		/* "KMRK" */
#define SIFT_KEY_MARKS_VERSION	1
#define SIFT_KEY_MARKS_HAS_HI	1U		/* the last key of every granule follows its first; else the marks are the first keys only */
#define SIFT_KEY_MARKS_EXACT	2U		/* no tuple of the marks was cut to key_bytes */

typedef struct SiftKeyMarksHdr
{
	uint32		magic;
	uint16		version;
	uint8		nkey_cols;		/* 1 .. SIFT_KEY_COLS_MAX */
	uint8		key_bytes;		/* the stride of a key: 8 .. 40, a multiple of 8 */
	uint32		ngranules;
	uint32		flags;
} SiftKeyMarksHdr;

StaticAssertDecl(sizeof(SiftKeyMarksHdr) == 16, "SiftKeyMarksHdr must be 16 bytes");

/* A section that passed sift_key_marks_open: pointers into its bytes, which stay valid as long as they do */
typedef struct SiftKeyMarks
{
	const uint8 *base;
	uint32		ngranules;
	uint8		nkey;
	uint8		key_bytes;
	bool		has_hi;
	bool		exact;
} SiftKeyMarks;

/* The first and the last key of granule g (the last: NULL where the section has first keys only) */
static inline const uint8 *
sift_key_marks_lo(const SiftKeyMarks *km, uint32 g)
{
	return km->base + (size_t) g * km->key_bytes * (km->has_hi ? 2 : 1);
}

static inline const uint8 *
sift_key_marks_hi(const SiftKeyMarks *km, uint32 g)
{
	return km->has_hi ? sift_key_marks_lo(km, g) + km->key_bytes : NULL;
}

/* The size of the section of n granules */
extern uint64 sift_key_marks_size(uint32 n, uint8 key_bytes, bool has_hi);

/* Checks the section against the spec of the key and the granules the part has (ngranules); NULL when it is sound, else what is wrong with it */
extern const char *sift_key_marks_open(const void *blob, uint64 len, const SiftKeySpec *spec, uint32 ngranules, SiftKeyMarks *km);

/* The marks of a keyed part ascend: the first keys, the last keys, and each granule's first not above its last, its last not above the next one's first; NULL when they do, else what breaks */
extern const char *sift_key_marks_check(const SiftKeyMarks *km);

/* The writer: add the first and the last row of each granule, in the order of the granules (vals: spec->ncols values each); finish returns the section (palloc'd) and frees the builder */
typedef struct SiftKeyMarksBuilder SiftKeyMarksBuilder;

extern SiftKeyMarksBuilder *sift_key_marks_begin(const SiftKeySpec *spec, bool has_hi);
extern void sift_key_marks_add(SiftKeyMarksBuilder *b, const SiftKeyVal *first, const SiftKeyVal *last);
extern void *sift_key_marks_finish(SiftKeyMarksBuilder *b, uint64 *len);

/* The section of parts joined as they are (their granules follow one another): the marks of n sections of one spec laid after each other; NULL when they differ in key or flags */
extern void *sift_key_marks_concat(const void *const *blobs, const uint64 *lens, int n, uint64 *len);

#endif							/* SIFT_KEY_MARKS_H */
