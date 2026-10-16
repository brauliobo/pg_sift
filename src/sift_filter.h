/*
 * sift_filter.h - a TIDBitmap of the heap tuples a filter keeps, as page and
 * offset masks per superblock (DESIGN.md, extras item 10).
 *
 * A ranked scan ANDs these masks into its candidate masks before scoring, so
 * documents the filter drops are never scored or verified. The mask is a
 * superset of the tuples that pass the filter (the bitmap's lossy pages keep
 * every offset), so the caller still evaluates the filter on the heap tuple.
 */
#ifndef SIFT_FILTER_H
#define SIFT_FILTER_H

#include "nodes/tidbitmap.h"

#include "sift.h"

/* The pages of one superblock; the offset masks of its exact pages are rows [row, row + popcount(pages & ~lossy)) */
typedef struct SiftFilterSB
{
	uint32		sb;
	uint32		row;
	uint64		pages[SIFT_PAGEMASK_WORDS];
	uint64		lossy[SIFT_PAGEMASK_WORDS];		/* pages of which every offset is kept */
} SiftFilterSB;

typedef struct SiftFilter
{
	SiftFilterSB *sb;				/* ascending sb */
	int64		nsb;
	uint64	  (*row)[SIFT_OFFMASK_WORDS];
	int64		nrow;
	int64		exact_pages;
	int64		lossy_pages;
	int64		tuples;				/* offsets kept in the exact pages */
	MemoryContext mcxt;
	int64		capsb;
	int64		caprow;
} SiftFilter;

/*
 * Converts the bitmap into masks allocated in mcxt, using at most maxbytes for them (the superblock
 * headers aside): pages that do not fit are kept as lossy pages. The bitmap is not modified.
 */
extern SiftFilter *sift_filter_build(TIDBitmap *tbm, Size maxbytes, MemoryContext mcxt);

static inline bool
sift_filter_empty(const SiftFilter *f)
{
	return f->nsb == 0;
}

/* The masks of superblock sb, NULL when the filter keeps none of its pages */
extern const SiftFilterSB *sift_filter_sb(const SiftFilter *f, uint32 sb);

/* Does the superblock keep a page of scoring block b (SIFT_BLK_PAGES pages) */
static inline bool
sift_filter_has_block(const SiftFilterSB *fs, int b)
{
	return (fs->pages[b / 4] >> ((b % 4) * SIFT_BLK_PAGES) & 0xffff) != 0;
}

/* Does the filter keep the tuple */
extern bool sift_filter_has(const SiftFilter *f, SiftKey key);

/* The offset masks of the SIFT_BLK_PAGES pages of block b, zero for the pages not kept */
extern void sift_filter_block(const SiftFilter *f, const SiftFilterSB *fs, int b, uint64 (*out)[SIFT_OFFMASK_WORDS]);

#endif							/* SIFT_FILTER_H */
