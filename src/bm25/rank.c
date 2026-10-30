/*
 * bm25/rank.c - the ranked search: term cursors, block bounds, accumulation and scoring of a block.
 */
#include "rank_int.h"

/*
 * Best-first search over (segment, SB) units ordered by their score upper
 * bound, then over the 16-page scoring blocks of a unit. A unit or block is
 * dropped once its bound is not above theta (the worst kept score, so equal
 * scores still compete on ctid). Candidates are scored exactly, verified by
 * the caller and only then enter the top-k; the sums follow the leaf order of
 * the bound query, as the reference does.
 */
void
seg_rewind(RankSeg *rs)
{
	ListCell   *lc;

	foreach(lc, rs->cs.posting)
		sift_post_rewind(lfirst(lc));
	rs->epoch++;
}


void
sift_rank_end(SiftRanker *r)
{
	hot_forget(r);
	for (int s = 0; s < r->plan->nsegs; s++)
		sift_sr_release(&r->plan->seg[s]->sr);
	MemoryContextDelete(r->tmpcx);
}

/* Fills the dense offset masks of the term's pages in block b; false when it has none */
static bool
term_mask(const sift_grp *g, int b, BlockMask out)
{
	uint64		pm = g->pages[b / 4] >> ((b % 4) * 16) & 0xffff;
	uint32		idx = 0;

	if (!pm)
		return false;
	for (int wd = 0; wd < b / 4; wd++)
		idx += pg_popcount64(g->pages[wd]);
	idx += pg_popcount64(g->pages[b / 4] & ((UINT64CONST(1) << ((b % 4) * 16)) - 1));
	memset(out, 0, sizeof(BlockMask));
	for (; pm; pm &= pm - 1, idx++)
		memcpy(out[pg_rightmost_one_pos64(pm)], g->off + (size_t) idx * g->w, g->w);
	return true;
}

/* tf of every posting of the term's group and the rank each block starts at */
static void
term_decode(MemoryContext cx, RkTerm *t)
{
	const sift_grp *g = t->g;
	const uint8 *off = g->off;
	uint32		n = 0;

	for (int b = 0; b < SIFT_BLKS_PER_SB; b++)
	{
		size_t		len = (size_t) pg_popcount64((g->pages[b / 4] >> (b % 4 * SIFT_BLK_PAGES)) & 0xffff) * g->w;

		t->base[b] = n;
		n += sift_popcount(off, len);
		off += len;
	}
	t->base[SIFT_BLKS_PER_SB] = n;
	if (n + SIFT_KERN_PAD > t->tfcap)
	{
		t->tfcap = Max(n + SIFT_KERN_PAD, 2 * t->tfcap);
		t->tf = t->tf ? repalloc(t->tf, t->tfcap) : MemoryContextAlloc(cx, t->tfcap);
	}
	sift_post_group_tf(t->gc, t->tf);
	t->decoded = true;
}

static inline bool
term_has(const RkTerm *t, int b)
{
	return t->g && (t->g->pages[b / 4] >> ((b % 4) * 16) & 0xffff) != 0;
}

/* Does the term score block b: it has postings there and can still matter (block_terms) */
static inline bool
term_in_block(const RkTerm *t, int b)
{
	return t->act && term_has(t, b);
}

/* The bounds of a term's SB gid */
const SiftGroupBounds *
term_bound_at(const RkTerm *t, uint32 gid)
{
	int			lo = 0,
				hi = t->ngb - 1;

	SIFT_BISECT(int, mid, lo, hi, t->gb[mid].gid < gid);
	Assert(t->gb[lo].gid == gid);
	return &t->gb[lo];
}

/* The score bound of the leaves in order, with leaf (when >= 0) bounded by v instead */
float4
sum_ub(const SiftPlan *p, const float4 *ub, int leaf, float4 v)
{
	float4		s = 0;

	for (int i = 0; i < p->nleaves; i++)
		s += i == leaf ? v : ub[i];
	return s;
}

/*
 * Marks the terms that can lift a document over theta in block b: a term
 * whose own bound, with the other leaves at their best, cannot exceed theta
 * never decides a selected score (the leaf takes the max over its terms), so
 * it is not scored. False when no term can.
 */
bool
block_terms(SiftPlan *p, RankSeg *rs, int b, float4 theta, float4 *ub)
{
	for (int i = 0; i < p->nleaves; i++)
	{
		RkTerm	   *t = rs->term + rs->first[i];

		ub[i] = 0;
		for (int j = 0; p->leaves[i].scoring && j < p->leaves[i].nterms[rs->s]; j++)
		{
			t[j].act = term_has(&t[j], b);
			if (t[j].act)
				ub[i] = Max(ub[i], t[j].ubw * (float4) t[j].bb->blk_bound[b]);
		}
	}
	if (!(sum_ub(p, ub, -1, 0) > theta))
		return false;
	for (int i = 0; i < p->nleaves; i++)
	{
		RkTerm	   *t = rs->term + rs->first[i];

		for (int j = 0; p->leaves[i].scoring && p->leaves[i].nterms[rs->s] > 1 && j < p->leaves[i].nterms[rs->s]; j++)
		{
			float4		v = t[j].ubw * (float4) (t[j].act ? t[j].bb->blk_bound[b] : 0);

			t[j].act = t[j].act && (v >= ub[i] || sum_ub(p, ub, i, v) > theta);
		}
	}
	return true;
}

/* Builds the term's mask in wk->tm, decoding its tf on first use */
static void
term_prepare(SiftPlan *p, RkTerm *t, int b, RankWork *wk)
{
	if (!t->decoded)
		term_decode(p->mcxt, t);
	term_mask(t->g, b, wk->tm);
}

static void
zero_rows(float4 *buf, const uint64 *mask)
{
	for (int pg = 0; pg < SIFT_BLK_PAGES; pg++)
		for (int k = 0; k < SIFT_OFFMASK_WORDS; k++)
			if (mask[pg * SIFT_OFFMASK_WORDS + k])
			{
				memset(buf + pg * SIFT_KERN_SLOTS, 0, SIFT_KERN_SLOTS * sizeof(float4));
				break;
			}
}

static void
add_rows(float4 *dst, const float4 *src, const uint64 *mask)
{
	for (int pg = 0; pg < SIFT_BLK_PAGES; pg++)
		for (int k = 0; k < SIFT_OFFMASK_WORDS; k++)
			for (uint64 m = mask[pg * SIFT_OFFMASK_WORDS + k]; m; m &= m - 1)
			{
				int			i = pg * SIFT_KERN_SLOTS + k * 64 + pg_rightmost_one_pos64(m);

				dst[i] += src[i];
			}
}

/* dst += this leaf's contribution in block b to the documents of cand */
static void
leaf_accum(SiftPlan *p, RankSeg *rs, int leaf, int b, const uint64 *cand, float4 *dst, RankWork *wk)
{
	RkTerm	   *t = rs->term + rs->first[leaf];
	int			nt = p->leaves[leaf].nterms[rs->s],
				np = 0;
	bool		first = true;

	for (int i = 0; i < nt; i++)
		np += term_in_block(&t[i], b);
	if (np == 0)
		return;
	if (np > 1)
	{
		memset(wk->lu, 0, sizeof(BlockMask));
		for (int i = 0; i < nt; i++)
			if (term_in_block(&t[i], b))
			{
				term_mask(t[i].g, b, wk->tm);
				sift_offmask_and(wk->tmc[0], wk->tm[0], cand, SIFT_BLK_PAGES);
				sift_offmask_or(wk->lu[0], wk->lu[0], wk->tmc[0], SIFT_BLK_PAGES);
			}
		zero_rows(wk->leafbuf, wk->lu[0]);
	}
	for (int i = 0; i < nt; i++)
	{
		float4	   *out = np == 1 ? dst : wk->leafbuf;

		if (!term_in_block(&t[i], b))
			continue;
		term_prepare(p, &t[i], b, wk);
		if (!first)
		{
			sift_offmask_and(wk->tmc[0], wk->tm[0], cand, SIFT_BLK_PAGES);
			zero_rows(wk->tmp, wk->tmc[0]);
			out = wk->tmp;
		}
		sift_kern->bm25_sel(out, wk->tm[0], cand, SIFT_BLK_PAGES, t[i].tf + t[i].base[b], wk->knorm, t[i].w);
		if (!first)
			sift_kern->max_merge(wk->leafbuf, wk->tmp, wk->tmc[0], SIFT_BLK_PAGES);
		first = false;
	}
	if (np > 1)
		add_rows(dst, wk->leafbuf, wk->lu[0]);
}

/* The result documents of block b are those the filter keeps; false when none is left */
static bool
filter_result(const SiftPlan *p, const SiftFilterSB *fs, int b, RankWork *wk)
{
	uint64		any = 0;

	sift_filter_block(p->filter, fs, b, wk->flt);
	for (int pg = 0; pg < SIFT_BLK_PAGES; pg++)
		for (int k = 0; k < SIFT_OFFMASK_WORDS; k++)
			any |= wk->res[pg][k] &= wk->flt[pg][k];
	return any != 0;
}

/*
 * The leaves present in the block (ub > 0) by descending bound, ties by leaf,
 * and rest[i], the sum of the bounds after ord[i]; returns their number.
 */
static int
leaf_order(const SiftPlan *p, const float4 *ub, int *ord, float4 *rest)
{
	int			n = 0;
	float4		sum = 0;

	for (int i = 0; i < p->nleaves; i++)
		if (ub[i] > 0)
		{
			int			j = n++;

			for (; j > 0 && ub[ord[j - 1]] < ub[i]; j--)
				ord[j] = ord[j - 1];
			ord[j] = i;
		}
	for (int k = n - 1; k >= 0; k--)
	{
		rest[k] = sum;
		sum += ub[ord[k]];
	}
	return n;
}

/*
 * Term-at-a-time over the leaves with the best bounds first: after each leaf
 * the documents that cannot reach theta even with every remaining leaf at its
 * bound leave the candidates (wk->res). The partial sums here add in bound
 * order and only decide that; the scores of the survivors are summed in leaf
 * order afterwards. The margin covers the rounding of the two orders (the
 * bounds carry their own slack). False when no candidate is left.
 */
static bool
prune_leaves(SiftPlan *p, RankSeg *rs, int b, float4 theta, RankWork *wk, const int *ord, const float4 *rest, int n)
{
	float4		margin = fabsf(theta) * (1.0f / 65536);

	zero_rows(wk->part, wk->res[0]);
	for (int k = 0; k < n - 1; k++)
	{
		leaf_accum(p, rs, ord[k], b, wk->res[0], wk->part, wk);
		if (!sift_kern->prune_gt(wk->res[0], wk->part, SIFT_BLK_PAGES, theta - rest[k] - margin))
			return false;
	}
	return true;
}

/*
 * Scores block b of the unit's SB for the group's result documents; returns
 * the candidates above theta in wk->slot / wk->vals. ub is the bound of each
 * leaf in the block.
 */
int
score_block(SiftRanker *r, RankSeg *rs, const sift_grp *g, const SiftFilterSB *fs, const SiftDocChunk *dc, int b,
			float4 theta, const float4 *ub)
{
	SiftPlan   *p = r->plan;
	RankWork   *wk = r->wk;
	SiftSegment *seg = p->seg[rs->s];
	bool		any = false;
	uint32		want = 0;
	int			n;

	if (!term_mask(g, b, wk->res))
		memset(wk->res, 0, sizeof(BlockMask));
	if (fs && !filter_result(p, fs, b, wk))
		return 0;
	for (int pg = 0; pg < SIFT_BLK_PAGES; pg++)
	{
		uint64		cand = 0;

		for (int k = 0; k < SIFT_OFFMASK_WORDS; k++)
			cand |= wk->res[pg][k];
		want |= (uint32) (cand != 0) << pg;
	}
	if (!want)
		return 0;
	sift_seg_block_dl(seg, dc, b, want, wk->dl);
	for (int pg = 0; pg < SIFT_BLK_PAGES; pg++)
	{
		uint64		cand = want >> pg & 1;

		if (wk->dl[pg] == NULL)
			wk->dl[pg] = wk->zero;
		if (dc->dirty && cand)
			cand = sift_seg_page_mask_live(seg, dc, b * SIFT_BLK_PAGES + pg, wk->res[pg]);
		any |= cand != 0;
	}
	if (!any)
		return 0;
	sift_kern->knorm(wk->knorm, wk->dl, wk->res[0], SIFT_BLK_PAGES, &p->bm);
	n = leaf_order(p, ub, r->ord, r->rest);
	if (n > 1 && theta > -INFINITY && !prune_leaves(p, rs, b, theta, wk, r->ord, r->rest, n))
		return 0;
	zero_rows(wk->scores, wk->res[0]);
	for (int i = 0; i < p->nleaves; i++)
		if (p->leaves[i].scoring)
			leaf_accum(p, rs, i, b, wk->res[0], wk->scores, wk);
	return sift_kern->select_gt(wk->slot, wk->vals, wk->scores, wk->res[0], SIFT_BLK_PAGES, theta);
}
