/*
 * bm25/count.c - count(*) of the visible matches and their pages.
 */
#include "core/host.h"

#include <math.h>

#include "bm25/eval.h"
#include "bm25/eval_int.h"
#include "bm25/kernels.h"
#include "bm25/wbuf.h"
#include "bm25/prefix.h"
#include "bm25/segment.h"

typedef struct CountCtx
{
	SiftPlan   *plan;
	SiftVis    *vis;
	SiftVerifyFn verify;
	void	   *arg;
	MemoryContext tmpcx;
	uint32		sb;				/* the superblock allvis belongs to */
	uint64		allvis[SIFT_PAGEMASK_WORDS];
} CountCtx;

/* The all-visible pages of superblock sb, read once per superblock */
static const uint64 *
allvisible(CountCtx *cc, uint32 sb)
{
	if (sb != cc->sb)
	{
		cc->sb = sb;
		sift_vis_sb_mask(cc->vis, sb, cc->allvis);
	}
	return cc->allvis;
}

/* Keeps the visible offsets of a page of candidates: the VM bit decides an all-visible page, else the heap */
static int
visible_offsets(CountCtx *cc, BlockNumber blk, uint64 *offmask)
{
	uint32		page = blk & (SIFT_SB_PAGES - 1);

	return allvisible(cc, sift_block_sb(blk))[page / 64] >> (page % 64) & 1 ? sift_offmask_popcount(offmask, 1) :
		sift_vis_filter_page(cc->vis, blk, offmask);
}

/*
 * Matches of a page with a phrase: the candidates the index does not decide are read and rechecked, the
 * others only checked for visibility; the offsets that pass stay in the mask
 */
static int
recheck_page(CountCtx *cc, SiftPageMatch *pm)
{
	uint64		sure[SIFT_OFFMASK_WORDS],
				any = 0;
	int			n = 0;

	memcpy(sure, pm->sure, sizeof(sure));
	for (int wd = 0; wd < SIFT_OFFMASK_WORDS; wd++)
	{
		any |= sure[wd];
		for (uint64 m = pm->offmask[wd] & ~sure[wd]; m; m &= m - 1)
		{
			ItemPointerData tid;
			text	   *doc = NULL;
			MemoryContext old = MemoryContextSwitchTo(cc->tmpcx);

			ItemPointerSet(&tid, pm->blk, wd * 64 + pg_rightmost_one_pos64(m) + 1);
			cc->plan->stats.candidates++;
			if (cc->verify(cc->arg, &tid, true, &doc) && doc_pass(cc->plan, doc, NULL))
				n++;
			else
				pm->offmask[wd] &= ~(m & -m);
			MemoryContextSwitchTo(old);
			MemoryContextReset(cc->tmpcx);
		}
	}
	if (any)
	{
		uint64		seen[SIFT_OFFMASK_WORDS];

		memcpy(seen, sure, sizeof(seen));
		n += visible_offsets(cc, pm->blk, sure);
		for (int wd = 0; wd < SIFT_OFFMASK_WORDS; wd++)
			pm->offmask[wd] &= ~(seen[wd] & ~sure[wd]);
	}
	return n;
}

/* The visible matches of the superblock ranges next hands out (next may be NULL: all), page by page */
static int64
eval_pages(SiftPlan *plan, SiftVis *vis, SiftVerifyFn verify, void *arg, SiftPageFn fn, void *fnarg,
		   SiftNextRangeFn next, void *next_arg)
{
	CountCtx	cc = {plan, vis, verify, arg, AllocSetContextCreate(CurrentMemoryContext, "sift count", ALLOCSET_DEFAULT_SIZES), UINT32_MAX};
	SiftMatchIter *it = sift_iter_begin(plan);
	SiftPageMatch pm;
	int64		n = 0;

	if (next)
		sift_iter_ranges(it, next, next_arg);

	while (sift_iter_next(it, &pm))
	{
		int			left;

		CHECK_FOR_INTERRUPTS();
		left = pm.recheck ? recheck_page(&cc, &pm) : visible_offsets(&cc, pm.blk, pm.offmask);
		if (left == 0)
			continue;
		n += left;
		fn(fnarg, pm.blk, pm.offmask);
	}
	sift_iter_end(it);
	MemoryContextDelete(cc.tmpcx);
	plan->stats.verified = n;
	return n;
}

int64
sift_eval_pages(SiftPlan *plan, SiftVis *vis, SiftVerifyFn verify, void *arg, SiftPageFn fn, void *fnarg)
{
	return eval_pages(plan, vis, verify, arg, fn, fnarg, NULL, NULL);
}

static void
count_page(void *arg, BlockNumber blk, const uint64 *offmask)
{
}

/*
 * The visible matches of one segment's group of a superblock, counted where they lie in the group: the
 * all-visible pages of a clean group in one pass, the others page by page. With delta, the live postings of the group
 * are counted already (the term's df over the segment) and the result is what the pages the heap decides add to
 * or take off them
 */
static int64
count_group(CountCtx *cc, SiftSegment *seg, const sift_grp *g, bool delta)
{
	const uint8 *off = g->off;
	const uint64 *allvis = allvisible(cc, g->gid);
	uint64		bulk[SIFT_PAGEMASK_WORDS];
	int64		n = 0;
	bool		dirty = (seg->entry.flags & SIFT_SEG_DIRTY) && sift_seg_sb_dirty(seg, g->gid);

	for (int wd = 0; wd < SIFT_PAGEMASK_WORDS; wd++)
		bulk[wd] = g->pages[wd] & allvis[wd];
	if (!delta)
		n = dirty ? sift_seg_live_count(seg, g, bulk) : sift_grp_count_mask(g, bulk);
	for (int wd = 0; wd < SIFT_PAGEMASK_WORDS && memcmp(bulk, g->pages, sizeof(bulk)) != 0; wd++)
		for (uint64 m = g->pages[wd]; m; m &= m - 1, off += g->w)
		{
			int			page = wd * 64 + pg_rightmost_one_pos64(m);
			uint64		mask[SIFT_OFFMASK_WORDS] = {0};

			if (bulk[wd] >> (page & 63) & 1)
				continue;
			memcpy(mask, off, g->w);
			if (dirty && sift_seg_live_mask(seg, g->gid, page, mask) == 0)
				continue;
			n -= delta ? sift_offmask_popcount(mask, 1) : 0;
			n += visible_offsets(cc, (BlockNumber) g->gid << SIFT_SB_SHIFT | page, mask);
		}
	sift_sr_release(&seg->sr);
	return n;
}

/*
 * The live postings of a plan that is one plain term, from the segments' counts of them (a term's df less its dead postings), or -1
 * when the plan is another or a segment cannot say: a segment with vacuumed docs of format 1, or whose table of dead postings a
 * VACUUM is changing
 */
static int64
count_by_df(const SiftPlan *plan)
{
	const EvalLeaf *l;
	int64		n = 0;

	if (plan->root->kind != EN_LEAF || plan->filter)
		return -1;
	l = &plan->leaves[plan->root->leaf];
	if (l->m != NULL || l->phrase != NULL)
		return -1;
	for (int s = 0; s < plan->nsegs; s++)
	{
		if (l->nterms[s] > 1 || !sift_seg_live_stable(plan->seg[s]))
			return -1;
		n += l->nterms[s] ? l->terms[s][0].info.ldf : 0;
	}
	return n;
}

/*
 * count(*) of a plan the bitmaps decide alone, without the write buffers' matches: no page is copied. A plan that is one term
 * starts from the segments' counts of its live postings and corrects them for the pages the heap decides, so a superblock with
 * vacuumed docs and all-visible pages costs what a clean one does; a VACUUM that changed a segment meanwhile has the count redone
 * from the liveness rows
 */
static int64
count_visible(SiftPlan *plan, SiftVis *vis, SiftNextRangeFn next, void *next_arg)
{
	CountCtx	cc = {.plan = plan,.vis = vis,.sb = UINT32_MAX};
	SiftMatchIter *it;
	int64		n;
	bool		delta;
	uint32		sb;

	SIFT_TEST_GATE(sift_test_count_gate, 1);
	n = next ? -1 : count_by_df(plan);
	delta = n >= 0;
retry:
	it = sift_iter_begin(plan);
	n = delta ? n : 0;
	if (next)
		sift_iter_ranges(it, next, next_arg);
	while (iter_find_sb(it, &sb))
	{
		for (int s = 0; s < plan->nsegs; s++)
			if (it->grp[s] && it->grp[s]->gid == sb)
				n += count_group(&cc, plan->seg[s], it->grp[s], delta);
		it->next_sb = sb + 1;
		plan->stats.sb_scanned++;
	}
	sift_iter_end(it);
	for (int s = 0; delta && s < plan->nsegs; s++)
		if (!sift_seg_live_stable(plan->seg[s]))
		{
			delta = false;
			goto retry;
		}
	plan->stats.verified = n;
	return n;
}

int64
sift_eval_count_ranges(SiftPlan *plan, SiftVis *vis, SiftVerifyFn verify, void *arg,
					   SiftNextRangeFn next, void *next_arg)
{
	if (!plan->heap && plan->npleaf == 0 && plan->nmhit == 0)
		return count_visible(plan, vis, next, next_arg);
	return eval_pages(plan, vis, verify, arg, count_page, NULL, next, next_arg);
}

int64
sift_eval_count(SiftPlan *plan, SiftVis *vis, SiftVerifyFn verify, void *arg)
{
	return sift_eval_count_ranges(plan, vis, verify, arg, NULL, NULL);
}
