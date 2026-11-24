/*
 * col/v1/v1_reader_int.h - the state of the index reader (col/v1/v1_reader.c), which the aggregate engine's source of an index starts with
 */
#ifndef SIFT_COL_V1_V1_READER_INT_H
#define SIFT_COL_V1_V1_READER_INT_H

#include "col/col.h"
#include "col/v1/src_sel.h"
#include "col/v1/src_wide.h"
#include "col/v1/v1_reader.h"

typedef struct SiftV1Idx
{
	SiftColSource *cs;
	SiftColUnit unit;
	SiftSrcLayout *lay;
	MemoryContext cxt;
	SiftSrcDict *dict;			/* per leaf: the dictionary of the last read */
	SiftSrcLeafBuf *buf;		/* per leaf */
	bool	   *tol;			/* per leaf: its values the index cannot hold are read as NULL */
	bool	   *raw;			/* per leaf: an image leaf read as the codes of its stored dictionary */
	Relation	heap;
	Relation	index;
	Snapshot	snapshot;
	SiftSrcSel	sel;			/* the match set the units are restricted to */
	SiftSrcWide *wide;			/* per leaf: the dictionary of a wide leaf in the unit (wgen: the unit it was made for) */
	uint64	   *wgen;
	uint64		ugen;			/* the unit the reader is at, counted */
	uint64		wseq;			/* dictionaries made, for their ids */
	/* makes the dictionary of a wide leaf from its lanes into w and r->dict[leaf]: sift_v1_idx_wide_dict, or a reader's own for the leaves it reads another way */
	void		(*wide_dict) (struct SiftV1Idx *r, int leaf, SiftSrcWide *w, const SiftColVec *v, uint32 n);
} SiftV1Idx;

/* r is zeroed storage the caller owns; the reader's allocations live in mcxt */
extern void sift_v1_idx_init(SiftV1Idx *r, Relation heap, Relation index, Snapshot snapshot, MemoryContext mcxt, uint32 flags, const void *view, const SiftKeyRange *keys);
extern void sift_v1_idx_wide_dict(SiftV1Idx *r, int leaf, SiftSrcWide *w, const SiftColVec *v, uint32 n);

/* a leaf read through the per-unit dictionary of datums (col/v1/src_wide.h) */
static inline bool
sift_v1_idx_wide(const SiftV1Idx *r, int leaf)
{
	return r->lay->leaf[leaf].wide && !r->raw[leaf];
}

extern void sift_v1_idx_close(void *impl);
extern const SiftSrcLayout *sift_v1_idx_source_layout(const void *impl);
extern uint32 sift_v1_idx_nsb(void *impl);
extern void sift_v1_idx_select(void *impl, const SiftFilter *keep);
extern bool sift_v1_idx_next(void *impl, SiftSrcUnit *unit);
extern void sift_v1_idx_live(void *impl, const SiftSrcUnit *unit, uint64 *bits);
extern bool sift_v1_idx_vet_needed(void *impl, const SiftSrcUnit *unit);
extern void sift_v1_idx_vet(void *impl, const SiftSrcUnit *unit, uint64 *bits);
extern void sift_v1_idx_tids(void *impl, const SiftSrcUnit *unit, const uint32 *rows, int n, ItemPointerData *tids);
extern int64 sift_v1_idx_leaf_n(void *impl, const SiftSrcUnit *unit, int leaf);
extern void sift_v1_idx_read(void *impl, const SiftSrcUnit *unit, int leaf, SiftSrcVec *vec);
extern void sift_v1_idx_read_into(void *impl, const SiftSrcUnit *unit, int leaf, const SiftSrcBuf *buf, SiftSrcVec *vec);
extern void sift_v1_idx_read_sel(void *impl, const SiftSrcUnit *unit, int leaf, const uint64 *bits, SiftSrcVec *vec);
extern void sift_v1_idx_read_rows(void *impl, const SiftSrcUnit *unit, int leaf, const uint32 *rows, int n, SiftSrcVec *vec);

#endif							/* SIFT_COL_V1_V1_READER_INT_H */
