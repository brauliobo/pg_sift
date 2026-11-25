/*
 * col/granule/cfilt/str_simd.h - the string predicates the chunks of a granule answer without a row at a time: <>, LIKE '%lit%', ILIKE '%lit%' and NOT LIKE '%lit%' on the bytes of a whole chunk (every
 * string of it is one slab, swept once by the substring search), the string-predicate kernels of col/granule/filter.c.
 */
#ifndef SIFT_GRANULE_CFILT_STR_SIMD_H
#define SIFT_GRANULE_CFILT_STR_SIMD_H

#include "col/am.h"
#include "col/granule/filter.h"
#include "col/like/simd.h"
#include "core/par/arena.h"

/* bits := the complement of bits over its first n bits, the bits past n zero */
extern void sift_cfs_invert(uint64 *bits, uint32 n);

/*
 * The bits of the rows of a string chunk that pass a <>, LIKE '%lit%', ILIKE '%lit%' or NOT LIKE '%lit%' predicate, for the values of dense_live (a bit for each value of the chunk, live_nn of them set;
 * NULL: all): the strings of the others are not decoded or tested and their bits are not defined; the chunks of strings that sit in the bytes of the record are tested in place and allocate nothing, and
 * a block or a FSST chunk keeps what it decoded in the memo (the live strings of a FSST chunk are decoded alone when they are few). false: the predicate or the chunk is one the caller decodes row by row.
 */
extern bool sift_cfstr_bits_live(const SiftCfiltPred *cp, const SiftChunk *c, const uint64 *dense_live, uint32 live_nn, uint64 *dense);

/* sift_cfstr_bits_live over all the values of the chunk */
extern bool sift_cfstr_bits(const SiftCfiltPred *cp, const SiftChunk *c, uint64 *dense);

/*
 * The strings a filter decoded whole (a block or a FSST chunk) are kept for the view of the same granule. A memo is the leader's (sift_cfstr_memo_open makes it in cx, and it is gone when cx is reset or
 * deleted; the functions without _in find it, and only on the backend thread) or a worker's: a SiftStrMemo the worker owns, zeroed, passed to the *_in and *_a functions, whose strings are the arena's, so a worker zeroes it whenever it
 * rewinds the arena. sift_cfstr_memo_strs_in gives the strings of the rows (ascending; NULL: the first nrows rows) of column col of v from the memo, false when the filter kept nothing for the column.
 */
#define SIFT_STR_MEMO_MAX	8

typedef struct SiftStrMemoEnt
{
	const char *rec;
	int			col;
	const uint32 *offs;
	const char *bytes;
	const uint64 *live;			/* the ranks that were decoded (the others are empty in offs), NULL: all */
} SiftStrMemoEnt;

struct SiftStrMemo
{
	MemoryContext cx;			/* the strings are chunks of cx; NULL: they are the arena's */
	int			n;
	SiftStrMemoEnt e[SIFT_STR_MEMO_MAX];
};

extern void sift_cfstr_memo_open(MemoryContext cx);
extern SiftStrMemo *sift_cfstr_memo_active(void);
extern bool sift_cfstr_memo_strs_in(const SiftStrMemo *m, const SiftGranuleView *v, int col, const uint16 *rows, uint16 nrows, SiftStrCell *out);

/* The same filter keeping what it decodes in m (NULL: nothing is kept) */
extern bool sift_cfstr_bits_live_in(SiftStrMemo *m, const SiftCfiltPred *cp, const SiftChunk *c, const uint64 *dense_live, uint32 live_nn, uint64 *dense);

/* On a worker: *handled is what sift_cfstr_bits_live returns; false, with err filled, is a damaged chunk or an arena that is full */
extern bool sift_cfstr_bits_live_a(SiftArena *a, SiftErr *err, SiftStrMemo *m, const SiftCfiltPred *cp, const SiftChunk *c, const uint64 *dense_live, uint32 live_nn, uint64 *dense, bool *handled);

#endif							/* SIFT_GRANULE_CFILT_STR_SIMD_H */
