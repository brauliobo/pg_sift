/*
 * bm25/iter.c - the cursor tree of a bound plan per segment and the iterator over its matches in heap order.
 */
#include "core/host.h"

#include <math.h>

#include "bm25/eval.h"
#include "bm25/eval_int.h"
#include "bm25/kernels.h"
#include "bm25/wbuf.h"
#include "bm25/prefix.h"
#include "bm25/segment.h"

/* ---------- iterator ---------- */

/* AND / OR of k >= 1 cursors; the single cursor itself for k = 1 */
static sift_cur *
op_cursor(SegCurs *cs, MemoryContext cx, EvalKind kind, sift_cur **kid, int k)
{
	sift_cur   *c;

	if (k == 1)
		return kid[0];
	c = MemoryContextAllocZero(cx, sizeof(sift_cur));
	if (kind == EN_AND)
		sift_cur_and(c, kid, k, cursor_buf(cx));
	else
		sift_cur_or(c, kid, k, cursor_buf(cx));
	if (cs)
		cs->ops = list_append_unique_ptr(cs->ops, c);
	return c;
}

static sift_cur *
leaf_cursor(SiftPlan *p, int s, const EvalLeaf *l, SegCurs *cs, const IterCurs *ic, MemoryContext cx)
{
	sift_cur  **kid = MemoryContextAlloc(cx, Max(l->nterms[s], 1) * sizeof(sift_cur *));
	SiftPostingCursor **pc = MemoryContextAlloc(cx, Max(l->nterms[s], 1) * sizeof(SiftPostingCursor *));
	bool		narrowed = cs && l->scoring && l->nterms[s] > 1;
	sift_cur   *orc = NULL;

	for (int i = 0; i < l->nterms[s]; i++)
	{
		const LeafTerm *t = &l->terms[s][i];

		pc[i] = l->wide ? sift_post_open_blob(&t->info, t->blob, cx) : sift_post_open(&p->seg[s]->sr, &t->info, cx);
		kid[i] = sift_post_cursor(pc[i]);
		if (cs && !narrowed)
			cs->posting = lappend(cs->posting, pc[i]);
	}
	if (l->nterms[s] > 0)
		orc = op_cursor(cs, cx, EN_OR, kid, l->nterms[s]);
	if (cs)
		cs->leafpc[l - p->leaves] = pc;
	else if (ic)
		ic->leafpc[l - p->leaves] = pc;
	if (narrowed)
		cs->leafor[l - p->leaves] = orc;
	return orc;
}

/* Cursor over the segment's matches of n, NULL when there are none; without cs, ic keeps every leaf's cursors */
sift_cur *
seg_cursor(SiftPlan *p, int s, const EvalNode *n, SegCurs *cs, const IterCurs *ic, MemoryContext cx)
{
	sift_cur  **pos,
			  **neg,
			   *c;
	int			npos = 0,
				nneg = 0;
	bool		dead = false;

	if (n->kind == EN_NOTHING)
		return NULL;
	if (n->kind == EN_LEAF)
		return n->phrase < 0 ? leaf_cursor(p, s, &p->leaves[n->leaf], cs, ic, cx) :
			phrase_cursor(p, s, n->phrase, leaf_cursor(p, s, &p->leaves[n->leaf], cs, ic, cx), ic, cx);
	pos = MemoryContextAlloc(cx, n->nchild * sizeof(sift_cur *));
	neg = MemoryContextAlloc(cx, n->nchild * sizeof(sift_cur *));
	for (int i = 0; i < n->nchild; i++)
	{
		const EvalNode *k = n->child[i];
		bool		isnot = k->kind == EN_NOT;

		c = seg_cursor(p, s, isnot ? k->child[0] : k, cs, ic, cx);
		if (c == NULL && n->kind == EN_AND && !isnot)
		{
			/* the ranker needs every leaf's cursors, even those of a branch that cannot match */
			if (cs == NULL)
				return NULL;
			dead = true;
		}
		if (c != NULL)
			(isnot ? neg : pos)[isnot ? nneg++ : npos++] = c;
	}
	if (npos == 0 || dead)
		return NULL;
	c = op_cursor(cs, cx, n->kind, pos, npos);
	if (n->phrase >= 0)
		c = phrase_cursor(p, s, n->phrase, c, ic, cx);
	if (nneg > 0)
	{
		sift_cur  **kid = MemoryContextAlloc(cx, 2 * sizeof(sift_cur *));
		sift_cur   *an = MemoryContextAllocZero(cx, sizeof(sift_cur));

		kid[0] = c;
		kid[1] = op_cursor(cs, cx, EN_OR, neg, nneg);
		sift_cur_andnot(an, kid, cursor_buf(cx));
		if (cs)
			cs->ops = lappend(cs->ops, an);
		c = an;
	}
	return c;
}

SiftMatchIter *
sift_iter_begin(SiftPlan *plan)
{
	SiftMatchIter *it = MemoryContextAllocZero(plan->mcxt, sizeof(SiftMatchIter));

	it->plan = plan;
	it->root = MemoryContextAllocZero(plan->mcxt, Max(plan->nsegs, 1) * sizeof(sift_cur *));
	it->grp = MemoryContextAllocZero(plan->mcxt, Max(plan->nsegs, 1) * sizeof(sift_grp *));
	it->om = MemoryContextAllocAligned(plan->mcxt, SIFT_SB_PAGES * SIFT_OFFMASK_WORDS * sizeof(uint64),
									   SIFT_SIMD_ALIGN, 0);
	it->page = SIFT_SB_PAGES;
	it->sb_end = UINT32_MAX;
	if (plan->nphr > 0)
	{
		it->pos = posctx_create(plan);
		it->curs = MemoryContextAllocZero(plan->mcxt, Max(plan->nsegs, 1) * sizeof(IterCurs));
	}
	if (plan->heap)
	{
		it->som = MemoryContextAllocAligned(plan->mcxt, SIFT_SB_PAGES * SIFT_OFFMASK_WORDS * sizeof(uint64),
											SIFT_SIMD_ALIGN, 0);
		it->sure = MemoryContextAllocZero(plan->mcxt, Max(plan->nsegs, 1) * sizeof(sift_cur *));
	}
	for (int s = 0; s < plan->nsegs; s++)
	{
		if (it->curs)
		{
			it->curs[s].leafpc = MemoryContextAllocZero(plan->mcxt, Max(plan->nleaves, 1) * sizeof(SiftPostingCursor **));
			it->curs[s].pos = it->pos;
		}
		it->root[s] = seg_cursor(plan, s, plan->root, NULL, it->curs ? &it->curs[s] : NULL, plan->mcxt);
		if (it->sure && plan->sure)
			it->sure[s] = seg_cursor(plan, s, plan->sure, NULL, NULL, plan->mcxt);
		sift_sr_release(&plan->seg[s]->sr);
	}
	return it;
}

void
sift_iter_ranges(SiftMatchIter *it, SiftNextRangeFn next, void *arg)
{
	it->next_range = next;
	it->range_arg = arg;
	it->sb_end = it->next_sb;
}

void
sift_iter_end(SiftMatchIter *it)
{
	for (int s = 0; s < it->plan->nsegs; s++)
		sift_sr_release(&it->plan->seg[s]->sr);
}

/* Points the phrase walkers at the groups the tree's cursors hold in SB gid: every term of a match is there */
static void
iter_phrase_groups(SiftMatchIter *it, int s, uint32 gid)
{
	const SiftPlan *p = it->plan;

	for (int k = 0; k < p->npleaf; k++)
	{
		SiftPostingCursor *pc = it->curs[s].leafpc[p->pleaf[k]][0];
		const sift_cur *c = sift_post_cursor(pc);

		if (c->state != 1 || c->g.gid != gid)
			elog(ERROR, "sift: a phrase term is not positioned at superblock %u", gid);
		posctx_group(it->pos, p->pleaf[k], pc, &c->g);
	}
}

/* Clears the docs of mask that lack a phrase the positions decide */
static void
phrases_filter(PosCtx *c, int s, int page, uint64 *mask)
{
	for (int k = 0; k < SIFT_OFFMASK_WORDS; k++)
		for (uint64 m = mask[k]; m; m &= m - 1)
			if (!phrases_hold(c, s, page, k * 64 + pg_rightmost_one_pos64(m)))
				mask[k] &= ~(m & -m);
}

/* ORs the offsets of mask into a page of an SB buffer (pages, om) */
static void
mask_or(uint64 *pages, uint64 (*om)[SIFT_OFFMASK_WORDS], int page, const uint64 *mask)
{
	if (!(pages[page / 64] & (UINT64CONST(1) << (page & 63))))
	{
		memset(om[page], 0, SIFT_OFFMASK_WORDS * sizeof(uint64));
		pages[page / 64] |= UINT64CONST(1) << (page & 63);
	}
	for (int k = 0; k < SIFT_OFFMASK_WORDS; k++)
		om[page][k] |= mask[k];
}

/*
 * ORs the live part of one segment's group into the SB buffer, minus the docs the positions refute. The sure
 * group (the sure tree's) goes to the buffer of the matches that need no recheck instead. So do the matches
 * of a segment whose positions decide every phrase, unless a negated phrase is left to the heap.
 */
static void
iter_merge(SiftMatchIter *it, int s, const sift_grp *g, bool sure)
{
	const SiftPlan *p = it->plan;
	SiftSegment *seg = p->seg[s];
	const uint8 *off = g->off;
	bool		dirty = (seg->entry.flags & SIFT_SEG_DIRTY) && sift_seg_sb_dirty(seg, g->gid);
	bool		pos = !sure && p->npleaf > 0,
				decided = !sure && it->som && !p->hardrecheck && !p->segheap[s];

	if (pos)
		iter_phrase_groups(it, s, g->gid);

	for (int wd = 0; wd < SIFT_PAGEMASK_WORDS; wd++)
		for (uint64 m = g->pages[wd]; m; m &= m - 1, off += g->w)
		{
			int			page = wd * 64 + pg_rightmost_one_pos64(m);
			uint64		mask[SIFT_OFFMASK_WORDS] = {0};
			bool		any = false;

			memcpy(mask, off, g->w);
			if (dirty)
				sift_seg_live_mask(seg, g->gid, page, mask);
			if (pos)
				phrases_filter(it->pos, s, page, mask);
			for (int k = 0; k < SIFT_OFFMASK_WORDS; k++)
				any |= mask[k] != 0;
			if (!any)
				continue;
			if (sure)
			{
				mask_or(it->spages, it->som, page, mask);
				continue;
			}
			if (p->nphr > 0 && p->segheap[s])
				it->heap[wd] |= UINT64CONST(1) << (page & 63);
			mask_or(it->pages, it->om, page, mask);
			if (decided)
				mask_or(it->spages, it->som, page, mask);
		}
	sift_sr_release(&seg->sr);
}

/* Sets the write buffer matches of SB sb in the SB buffer */
static void
iter_merge_wbuf(SiftMatchIter *it, uint32 sb)
{
	const SiftPlan *p = it->plan;

	for (; it->mpos < p->nmhit && sift_block_sb(sift_key_block(p->mhit[it->mpos].key)) == sb; it->mpos++)
	{
		SiftKey		key = p->mhit[it->mpos].key;
		int			page = sift_key_block(key) & (SIFT_SB_PAGES - 1),
					off = (key & 0xFFFF) - 1;

		if (!(it->pages[page / 64] & (UINT64CONST(1) << (page & 63))))
		{
			memset(it->om[page], 0, SIFT_OFFMASK_WORDS * sizeof(uint64));
			it->pages[page / 64] |= UINT64CONST(1) << (page & 63);
		}
		it->om[page][off / 64] |= UINT64CONST(1) << (off & 63);
		if (p->nphr > 0)
			it->heap[page / 64] |= UINT64CONST(1) << (page & 63);
	}
}

/*
 * Seeks the segments' cursors to the next SB with matches in any of them or in the write buffers
 * (it->grp[s] are their groups there); false at the end
 */
bool
iter_find_sb(SiftMatchIter *it, uint32 *sbp)
{
	for (;;)
	{
		uint32		sb = UINT32_MAX;

		if (it->next_range && it->next_sb >= it->sb_end)
		{
			uint32		lo,
						hi;

			if (!it->next_range(it->range_arg, &lo, &hi))
				return false;
			it->next_sb = lo;
			it->sb_end = hi;
			while (it->mpos < it->plan->nmhit && sift_block_sb(sift_key_block(it->plan->mhit[it->mpos].key)) < lo)
				it->mpos++;
		}
		for (int s = 0; s < it->plan->nsegs; s++)
		{
			it->grp[s] = it->root[s] ? sift_seek(it->root[s], it->next_sb) : NULL;
			if (it->grp[s] && it->grp[s]->gid < sb)
				sb = it->grp[s]->gid;
		}
		if (it->mpos < it->plan->nmhit)
			sb = Min(sb, sift_block_sb(sift_key_block(it->plan->mhit[it->mpos].key)));
		if (sb >= it->sb_end)
		{
			if (!it->next_range)
				return false;
			it->next_sb = it->sb_end;
			continue;
		}
		CHECK_FOR_INTERRUPTS();
		*sbp = sb;
		return true;
	}
}

/* Loads the next SB with matches in any segment; false at the end */
static bool
iter_load_sb(SiftMatchIter *it)
{
	uint32		sb;

	while (iter_find_sb(it, &sb))
	{
		memset(it->pages, 0, sizeof(it->pages));
		memset(it->heap, 0, sizeof(it->heap));
		memset(it->spages, 0, sizeof(it->spages));
		for (int s = 0; s < it->plan->nsegs; s++)
			if (it->grp[s] && it->grp[s]->gid == sb)
			{
				const sift_grp *sg = it->sure && it->sure[s] ? sift_seek_eq(it->sure[s], sb) : NULL;

				iter_merge(it, s, it->grp[s], false);
				if (sg)
					iter_merge(it, s, sg, true);
			}
		iter_merge_wbuf(it, sb);
		it->next_sb = sb + 1;
		it->plan->stats.sb_scanned++;
		if (it->pages[0] | it->pages[1] | it->pages[2] | it->pages[3])
		{
			it->sb = sb;
			it->page = 0;
			return true;
		}
	}
	return false;
}

/* Next set page at or after it->page; SIFT_SB_PAGES when none */
static int
iter_next_page(const SiftMatchIter *it)
{
	for (int wd = it->page / 64; wd < SIFT_PAGEMASK_WORDS; wd++)
	{
		uint64		m = it->pages[wd] & (wd == it->page / 64 ? ~UINT64CONST(0) << (it->page & 63) : ~UINT64CONST(0));

		if (m)
			return wd * 64 + pg_rightmost_one_pos64(m);
	}
	return SIFT_SB_PAGES;
}

bool
sift_iter_next(SiftMatchIter *it, SiftPageMatch *out)
{
	int			page = iter_next_page(it);

	if (page == SIFT_SB_PAGES)
	{
		if (!iter_load_sb(it))
			return false;
		page = iter_next_page(it);
	}
	out->blk = (BlockNumber) it->sb << SIFT_SB_SHIFT | page;
	memcpy(out->offmask, it->om[page], sizeof(out->offmask));
	memset(out->sure, 0, sizeof(out->sure));
	out->recheck = it->plan->hardrecheck || (it->heap[page / 64] >> (page & 63) & 1);
	if (out->recheck && it->spages[page / 64] >> (page & 63) & 1)
	{
		uint64		undecided = 0;

		memcpy(out->sure, it->som[page], sizeof(out->sure));
		for (int k = 0; k < SIFT_OFFMASK_WORDS; k++)
			undecided |= out->offmask[k] & ~out->sure[k];
		out->recheck = undecided != 0;
	}
	it->page = page + 1;
	return true;
}
