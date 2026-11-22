/*
 * col/f2/f2_proj_overlay.h - what a projection part answers for a base part that has changed since the link was made: the pure rules of f2_proj.h's sift_proj_serve_part and the exact sets of rows an
 * overlay takes away. Nothing here reads a page or knows a host: the host hands in the facts of the part, the dead rows of its deletion vector, the pending deletes of its partition and the heap
 * blocks that are not all-visible, and gets back what to read and which rows of a granule to take away, each once.
 */
#ifndef SIFT_F2_PROJ_OVERLAY_H
#define SIFT_F2_PROJ_OVERLAY_H

#include "core/host.h"

#include "col/f2/delvec.h"
#include "col/f2/f2_proj.h"
#include "col/f2/pend_del.h"

/* What the host knows of a part's visibility before it asks the rules: the inserters, the deletion vector, and for a sift_col the heap blocks of the part that are not all-visible */
typedef struct SiftProjVisIn
{
	bool		inserts_settled;	/* every inserter committed and older than the snapshot (sift_vis_part_inserts_settled) */
	bool		has_dv;
	bool		dv_settled;			/* every deleter of the vector committed and older than the snapshot */
	bool		heap_host;			/* a sift_col: the heap decides the rows the part cannot */
	bool		sort_kind;
	bool		has_envelope;		/* the part has an address range (tid_max != 0) */
	int			ndirty_blocks;		/* heap_host and AGG: the blocks of the range that are not all-visible, -1 when the range is too wide to inspect */
} SiftProjVisIn;

extern SiftProjHostVis sift_proj_vis_class(const SiftProjVisIn *in);

/* A pending delete of the partition may name a row of the part: its address range meets the part's (no page of either is read) */
extern bool sift_proj_part_meets_pend(const SiftPartDesc *base, const SiftPendDelIndex *px);

/* The heap blocks of a part that are not all-visible, ascending, and the bloom bits of them as SEC_TID_INDEX has them */
typedef struct SiftProjDirty
{
	const BlockNumber *blk;
	int			n;
	uint64		bloom;
} SiftProjDirty;

extern bool sift_proj_dirty_has(const SiftProjDirty *d, BlockNumber blk);
extern uint64 sift_proj_dirty_bloom(const BlockNumber *blk, int n);

/* Whether a granule can hold a row that has died since the link: by the deletion vector, a pending delete, or a heap block that is not all-visible, each told in a comparison of memory */
extern bool sift_proj_overlay_granule_hit(bool dv_has_granule, const SiftPendDelIndex *pend, const SiftProjDirty *dirty, const SiftTidIndexEntry *tix);

typedef bool (*SiftProjHeapVisibleFn) (void *arg, uint64 tid);

/* The rows of one granule that are dead for the snapshot and were not when the projection part was written */
typedef struct SiftProjOverlayIn
{
	uint32		nrows;
	const uint64 *tids;				/* one address for each row; NULL when neither pend nor dirty is set */
	const uint64 *dv_dead;			/* the rows the deletion vector now has dead for the snapshot (bit r); NULL: none */
	const uint64 *baseline_dead;	/* the rows the vector the part was written from had dead: already out of the cells; NULL: none */
	const SiftPendDelIndex *pend;	/* the pending deletes and the deletes of this transaction the snapshot sees */
	SiftProjDirty dirty;
	SiftProjHeapVisibleFn visible;	/* the heap row at the address is visible to the snapshot (a HOT chain followed) */
	void	   *arg;
} SiftProjOverlayIn;

/* mask: SIFT_DV_MASK_WORDS words, cleared and set here; returns the rows set. A row is in it at most once whatever sets of the three sources it is in. */
extern int	sift_proj_overlay_mask(const SiftProjOverlayIn *in, uint64 *mask);

/* The dead rows of a part are few enough for an overlay to meet (a SORT part by addresses, an AGG part by rows) */
static inline bool
sift_proj_sort_sparse(const SiftPartDesc *b)
{
	return b->ndead <= SIFT_PROJ_SORT_DV_OVERLAY_MAX && b->ndead * SIFT_PROJ_OVERLAY_DEAD_DENOM <= b->nrows;
}

static inline bool
sift_proj_agg_sparse(const SiftPartDesc *b)
{
	return b->ndead * SIFT_PROJ_OVERLAY_DEAD_DENOM <= b->nrows;
}

/* SORT: the address is out of the answer (dead in the vector, or pending), tested on a hash and no page */
static inline bool
sift_proj_sort_row_dead(const SiftPendDelIndex *dv_dead, const SiftPendDelIndex *pend, uint64 tid)
{
	return (dv_dead != NULL && sift_pdel_idx_has(dv_dead, tid)) || (pend != NULL && sift_pdel_idx_has(pend, tid));
}

#endif							/* SIFT_F2_PROJ_OVERLAY_H */
