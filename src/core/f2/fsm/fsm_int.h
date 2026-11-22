/*
 * core/f2/fsm/fsm_int.h - what the files of the allocator of format major 2 share (core/f2/fsm.c, fsm/slab.c, fsm/release_guard.c, fsm/check.c): the page layouts, the context of a call and the walk of a list.
 */
#ifndef SIFT_CORE_F2_FSM_INT_H
#define SIFT_CORE_F2_FSM_INT_H

#include "core/host.h"

#include "core/f2/fsm.h"
#include "core/page_io.h"

#define FSMM(page)	((SiftFsmMetaPageData *) SiftPageGetData(page))
#define SLABS(page)	((SiftFsmSlabTable *) (SiftPageGetData(page) + SIFT_FSM_SLABS_OFF))
#define FSMP(page)	((SiftFsmPageData *) SiftPageGetData(page))

#define SIFT_FSM_META_RESERVE	16	/* free metadata blocks kept in the slabs: a step that writes a list page never finds none, and no slab is given back that would leave fewer */
#define SIFT_FSM_KEEP_SLABS		2	/* the slabs a quiet relation keeps: the first, and one warm one so that a burst does not claim and give back a slab in turn */

/* One call of the allocator: block 0 held exclusively by the caller */
typedef struct FsmCtx
{
	Relation	rel;
	Buffer		buf0;
	bool		exclusive;		/* the caller holds AccessExclusiveLock on the relation: no scan, and so no reader of an older manifest, exists, whatever the safexid of a retired extent (core/freelist.c, space mode) */
	bool		in_reclaim;		/* reclaim_locked is under way: it needs pages of the slabs itself and must not be asked to reclaim from inside */
	FullTransactionId horizon;	/* valid: the horizon computed anew, what the extents are matured against instead of the cached GlobalVis one */
} FsmCtx;

typedef struct ExtVec
{
	SiftFsmExtent *v;
	int			n;
	int			max;
} ExtVec;

extern void sift_fsm_extvec_add(ExtVec *e, BlockNumber start, uint32 nblocks, FullTransactionId safexid);
extern int	sift_fsm_ext_cmp(const void *a, const void *b);
extern void sift_fsm_check_root(Relation rel, Buffer buf);
extern void sift_fsm_walk_list(Relation rel, BlockNumber head, uint16 size_class, ExtVec *out, ExtVec *pages);

/* fsm.c: what the slab code needs of the lists and of block 0 */
extern Buffer sift_fsm_lock_root(Relation rel);
extern void sift_fsm_root_seal(Page page);
extern SiftFsmExtent sift_fsm_pop_class(FsmCtx *c, int cls);
extern void sift_fsm_push_class(FsmCtx *c, int cls, const SiftFsmExtent *ext, int n);
extern BlockNumber sift_fsm_high_water(FsmCtx *c);
extern void sift_fsm_set_high_water(FsmCtx *c, SiftXlog *x, BlockNumber high_water);
extern uint64 sift_fsm_reclaim_locked(FsmCtx *c, BlockNumber *new_hw);

/* fsm/slab.c: the bitmaps of the metadata slabs and the slabs themselves */
extern SiftFsmSlab *sift_fsm_slab_of(SiftFsmSlabTable *t, BlockNumber blk);
extern void sift_fsm_slab_init(SiftFsmSlab *s, BlockNumber start, uint32 nblocks);
extern BlockNumber sift_fsm_slab_take(Page root);
extern void sift_fsm_slab_put(Relation rel, Page root, BlockNumber blk);
extern void sift_fsm_claim_slab(FsmCtx *c);
extern BlockNumber sift_fsm_alloc_meta_locked(FsmCtx *c);

/* Makes metadata blocks available: the matured pages of the queue go back to the bitmaps first, a new slab is claimed only when the free blocks are still under the reserve */
extern void sift_fsm_grow_meta(FsmCtx *c);
extern bool sift_fsm_below_reserve(FsmCtx *c);

/* The slabs past the second whose every block is free go back to the data zone (one record each); the number of blocks returned */
extern uint32 sift_fsm_recycle_empty_slabs(FsmCtx *c);

/* The slabs with no page in use, as many as leave the reserve free in the others, for the shrink of the file that cuts the tail; dropped from the root with sift_fsm_slabs_drop */
extern void sift_fsm_slabs_idle(const SiftFsmSlabTable *t, uint32 meta_free, ExtVec *out);
extern void sift_fsm_slabs_drop(Page root, const ExtVec *idle);

/* fsm/release_guard.c: a release that names a block twice, or one that is no data or metadata block, is an error at the caller that made it */
extern void sift_fsm_release_guard(Relation rel, Page root, const SiftExtent *ext, int n);

#endif
