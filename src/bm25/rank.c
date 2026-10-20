/*
 * bm25/rank.c - the ranked search: best-first over units and blocks, scoring, verification and the top-k.
 */
#include "core/host.h"

#include <math.h>

#include "bm25/eval.h"
#include "bm25/eval_int.h"
#include "bm25/kernels.h"
#include "bm25/wbuf.h"
#include "bm25/prefix.h"
#include "bm25/segment.h"

/* ---------- ranker ---------- */

/*
 * Best-first search over (segment, SB) units ordered by their score upper
 * bound, then over the 16-page scoring blocks of a unit. A unit or block is
 * dropped once its bound is not above theta (the worst kept score, so equal
 * scores still compete on ctid). Candidates are scored exactly, verified by
 * the caller and only then enter the top-k; the sums follow the leaf order of
 * the bound query, as the reference does.
 */

static void hot_forget(SiftRanker *r);

static void
seg_rewind(RankSeg *rs)
{
	ListCell   *lc;

	foreach(lc, rs->cs.posting)
		sift_post_rewind(lfirst(lc));
	rs->epoch++;
}

/* The cursors of the narrowed leaves rewind when used first after a rewind of the segment */
static inline void
term_sync(RankSeg *rs, SiftPostingCursor *pc, uint32 *epoch)
{
	if (*epoch != rs->epoch)
		sift_post_rewind(pc);
	*epoch = rs->epoch;
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
static const SiftGroupBounds *
term_bound_at(const RkTerm *t, uint32 gid)
{
	int			lo = 0,
				hi = t->ngb - 1;

	while (lo < hi)
	{
		int			mid = (lo + hi) / 2;

		if (t->gb[mid].gid < gid)
			lo = mid + 1;
		else
			hi = mid;
	}
	Assert(t->gb[lo].gid == gid);
	return &t->gb[lo];
}

/* The score bound of the leaves in order, with leaf (when >= 0) bounded by v instead */
static float4
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
static bool
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
static int
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

/* Index entry of a hit whose visible tuple sits elsewhere on the page (HOT chain) */
typedef struct HotEnt
{
	SiftKey		vis;
	SiftKey		root;
} HotEnt;

static void
hot_remember(SiftRanker *r, SiftKey vis, SiftKey root)
{
	if (r->hot == NULL)
	{
		HASHCTL		ctl = {.keysize = sizeof(SiftKey), .entrysize = sizeof(HotEnt), .hcxt = r->plan->mcxt};

		r->hot = hash_create("sift hot chains", 16, &ctl, HASH_ELEM | HASH_BLOBS | HASH_CONTEXT);
	}
	((HotEnt *) hash_search(r->hot, &vis, HASH_ENTER, NULL))->root = root;
}

static void
hot_forget(SiftRanker *r)
{
	if (r->hot)
		hash_destroy(r->hot);
	r->hot = NULL;
}

/*
 * Verifies one candidate and keeps it when it is among the best. Ties go by the ctid of the visible
 * tuple, which shares the index entry's page but not its offset along a HOT chain: the first check
 * therefore compares the page only.
 */
static void
rank_candidate(SiftRanker *r, SiftTopK *tk, SiftKey key, float4 score, int seg)
{
	SiftPlan   *p = r->plan;
	ItemPointerData tid;
	SiftKey		vis;
	text	   *doc = NULL;
	MemoryContext old;
	bool		ok,
				heap = p->recheck || p->nphr > 0;

	if (!sift_kern_topk_admits(tk, score, (key & ~UINT64CONST(0xFFFF)) | 1))
		return;
	if (seg >= 0 && r->posok)
	{
		if (!phrases_hold(r->phrpos, seg, sift_key_block(key) & (SIFT_SB_PAGES - 1), (key & 0xFFFF) - 1))
			return;
		heap = p->recheck || p->segheap[seg];
	}
	sift_key_to_tid(key, &tid);
	p->stats.candidates++;
	old = MemoryContextSwitchTo(r->tmpcx);
	ok = r->verify(r->arg, &tid, heap, &doc);
	if (ok && heap)
		ok = doc_pass(p, doc, &score);
	MemoryContextSwitchTo(old);
	MemoryContextReset(r->tmpcx);
	if (!ok)
		return;
	p->stats.verified++;
	vis = sift_key_from_tid(&tid);
	if (!sift_kern_topk_admits(tk, score, vis))
		return;
	if (tk->n == tk->cap && tk->n < tk->k)
		tk->hit = repalloc_huge(tk->hit, (tk->cap = Min(tk->k, 2 * tk->cap)) * sizeof(SiftHit));
	sift_kern_topk_push(tk, score, vis);
	if (vis != key)
		hot_remember(r, vis, key);
}

/* Blocks by descending bound, ties by block number: at most 16 entries, so an insertion sort */
static void
order_blocks(int *order, int n, const float4 *ub)
{
	for (int i = 1; i < n; i++)
	{
		int			b = order[i],
					j = i;

		for (; j > 0 && ub[order[j - 1]] < ub[b]; j--)
			order[j] = order[j - 1];
		order[j] = b;
	}
}

static sift_cur *
term_cursor(RankSeg *rs, RkTerm *t)
{
	term_sync(rs, t->pc, &t->pce);
	return sift_post_cursor(t->pc);
}

/*
 * A term of a multi-term leaf that cannot lift a score over theta anywhere
 * (its best ub with the other leaves at theirs) neither matches nor scores:
 * the documents it alone matches cannot be selected. The leaf's OR holds the
 * other terms only (never none: an empty OR is not a cursor).
 */
static void
unit_terms(SiftPlan *p, RankSeg *rs, float4 theta)
{
	for (int i = 0; i < p->nleaves; i++)
	{
		RkTerm	   *t = rs->term + rs->first[i];
		sift_cur   *orc = rs->cs.leafor[i];
		int			nt = p->leaves[i].nterms[rs->s],
					nlive = 0;

		for (int j = 0; p->leaves[i].scoring && j < nt; j++)
		{
			t[j].g = NULL;
			t[j].decoded = false;
			t[j].live = nt == 1 || sum_ub(p, rs->lmax, i, t[j].tub) > theta;
			if (orc && t[j].live)
				orc->op.kid[nlive++] = term_cursor(rs, &t[j]);
		}
		if (orc && nlive == 0)
			orc->op.kid[nlive++] = term_cursor(rs, &t[0]);
		if (orc)
			orc->op.k = nlive;
	}
}

/* Points the phrase walkers at the unit's groups; false when a term has none there */
static bool
rank_phrase_groups(SiftRanker *r, const RankSeg *rs)
{
	for (int k = 0; k < r->plan->npleaf; k++)
	{
		const RkTerm *t = rs->term + rs->first[r->plan->pleaf[k]];

		if (t->g == NULL)
			return false;
		posctx_group(r->phrpos, r->plan->pleaf[k], t->gc, t->g);
	}
	return true;
}

/* The term's own cursor at the unit's SB; the tree's seeks skip groups of branches that do not match, which still score */
static void
term_seek(SiftPlan *p, RankSeg *rs, RkTerm *t, uint32 gid)
{
	sift_cur   *tc = sift_post_cursor(t->pc);
	const sift_grp *g;

	if (tc->state == 1 && tc->g.gid == gid)
		t->gc = t->pc;
	else
	{
		if (t->sc == NULL)
			t->sc = sift_post_clone(t->pc, p->mcxt);
		term_sync(rs, t->sc, &t->sce);
		t->gc = t->sc;
	}
	g = sift_seek(sift_post_cursor(t->gc), gid);
	if (g && g->gid == gid)
	{
		t->g = g;
		t->bb = term_bound_at(t, gid);
	}
}

/*
 * What a candidate must beat: the worst kept score, and in a parallel search
 * the best of the participants' (the k-th best of any subset is at most the
 * k-th best of all, so nothing at or below it is among the k best)
 */
static inline float4
rank_theta(const SiftRanker *r, const SiftTopK *tk)
{
	float4		theta = sift_kern_topk_theta(tk);

	return r->par_active ? Max(theta, sift_par_theta(r->par)) : theta;
}

static void
rank_unit(SiftRanker *r, const Unit *u, SiftTopK *tk)
{
	SiftPlan   *p = r->plan;
	RankSeg    *rs = &r->seg[u->seg];
	SiftSegment *seg = p->seg[u->seg];
	const sift_grp *g;
	const SiftFilterSB *fs;
	SiftDocChunk dc;
	float4		theta = rank_theta(r, tk);
	int			order[SIFT_BLKS_PER_SB],
				n = 0;

	if ((int64) u->gid < rs->last)
		seg_rewind(rs);
	rs->last = u->gid;
	unit_terms(p, rs, theta);
	for (ListCell *lc = list_head(rs->cs.ops); lc; lc = lnext(rs->cs.ops, lc))
		((sift_cur *) lfirst(lc))->state = 0;
	g = sift_seek_eq(rs->root, u->gid);
	p->stats.sb_scanned++;
	if (!g || !sift_seg_docs(seg, u->gid, &dc))
	{
		sift_sr_release(&seg->sr);
		return;
	}
	fs = p->filter ? sift_filter_sb(p->filter, u->gid) : NULL;
	Assert(fs || !p->filter);
	for (int b = 0; b < SIFT_BLKS_PER_SB; b++)
		if (g->pages[b / 4] >> ((b % 4) * 16) & 0xffff && (fs == NULL || sift_filter_has_block(fs, b)))
			order[n++] = b;
	if (n == 0)
	{
		sift_sr_release(&seg->sr);
		return;
	}
	for (int i = 0; i < p->nleaves; i++)
		for (int j = 0; p->leaves[i].scoring && j < p->leaves[i].nterms[u->seg]; j++)
			if (rs->term[rs->first[i] + j].live)
				term_seek(p, rs, &rs->term[rs->first[i] + j], u->gid);
	r->posok = p->nphr > 0 && rank_phrase_groups(r, rs);
	order_blocks(order, n, u->ub);
	for (int i = 0; i < n; i++)
	{
		int			b = order[i],
					nsel;

		theta = rank_theta(r, tk);
		if (!(u->ub[b] > theta) || !block_terms(p, rs, b, theta, r->leafub))
		{
			p->stats.blocks_skipped++;
			continue;
		}
		p->stats.blocks_scored++;
		nsel = score_block(r, rs, g, fs, &dc, b, theta, r->leafub);
		sift_sr_release(&seg->sr);
		for (int j = 0; j < nsel; j++)
		{
			int			slot = r->wk->slot[j];
			BlockNumber blk = u->gid << SIFT_SB_SHIFT | (b * SIFT_BLK_PAGES + slot / SIFT_KERN_SLOTS);

			rank_candidate(r, tk, (SiftKey) blk << 16 | (slot % SIFT_KERN_SLOTS + 1), r->wk->vals[j], u->seg);
		}
		if (r->par_active)
			sift_par_raise_theta(r->par, sift_kern_topk_theta(tk));
	}
	sift_sr_release(&seg->sr);
}

static void
rank_skip_units(SiftRanker *r, int64 from, int64 to)
{
	for (; from < to; from++)
		r->plan->stats.blocks_skipped += r->unit[from].nblk;
}

static void
rank_wbuf(SiftRanker *r, SiftTopK *tk)
{
	const SiftPlan *p = r->plan;

	for (int64 i = 0; i < p->nmhit; i++)
		if (p->filter == NULL || sift_filter_has(p->filter, p->mhit[i].key))
			rank_candidate(r, tk, p->mhit[i].key, p->mhit[i].score, -1);
}

static void
rank_units(SiftRanker *r, SiftTopK *tk)
{
	int			u;

	rank_wbuf(r, tk);
	for (u = 0; u < r->nunit; u++)
	{
		if (!(r->unit[u].maxub > sift_kern_topk_theta(tk)))
			break;
		CHECK_FOR_INTERRUPTS();
		rank_unit(r, &r->unit[u], tk);
	}
	rank_skip_units(r, u, r->nunit);
}

/*
 * The units are handed out in descending bound, so once one is out of reach
 * of the shared threshold the rest of the list is too; this participant is
 * done, and whoever claims on finds the same.
 */
static void
rank_units_shared(SiftRanker *r, SiftTopK *tk)
{
	int64		lo,
				hi;
	bool		reachable = true;

	r->par_active = true;
	if (sift_par_claim_wbuf(r->par))
		rank_wbuf(r, tk);
	while (reachable && sift_par_next_units(r->par, r->nunit, &lo, &hi))
		for (int64 u = lo; u < hi && reachable; u++)
		{
			CHECK_FOR_INTERRUPTS();
			if ((reachable = r->unit[u].maxub > rank_theta(r, tk)))
				rank_unit(r, &r->unit[u], tk);
			else
				rank_skip_units(r, u, hi);
		}
	r->par_active = false;
}

static int
par_hit_cmp(const void *a, const void *b)
{
	const SiftParHit *x = a,
			   *y = b;

	if (x->score != y->score)
		return x->score > y->score ? -1 : 1;
	return pg_cmp_u64(x->key, y->key);
}

/* Publishes the participant's sorted hits; the last participant takes everyone's hits as the result */
static void
rank_merge_shared(SiftRanker *r)
{
	SiftParHit *mine = palloc_extended(Max(r->nhit, 1) * sizeof(SiftParHit), MCXT_ALLOC_HUGE),
			   *all;
	int64		n;

	for (int64 i = 0; i < r->nhit; i++)
	{
		HotEnt	   *e = r->hot ? hash_search(r->hot, &r->hit[i].key, HASH_FIND, NULL) : NULL;

		mine[i] = (SiftParHit) {r->hit[i].key, e ? e->root : r->hit[i].key, r->hit[i].score};
	}
	sift_par_publish(r->par, mine, r->nhit);
	pfree(mine);
	r->nhit = 0;
	r->complete = true;
	if (!sift_par_finish(r->par))
		return;
	all = sift_par_collect(r->par, &n);
	qsort(all, n, sizeof(SiftParHit), par_hit_cmp);
	hot_forget(r);
	r->nhit = Min(n, r->K);
	r->complete = n < r->K;
	if (r->nhit > r->hitcap)
		r->hit = repalloc_huge(r->hit, (r->hitcap = r->nhit) * sizeof(SiftHit));
	for (int64 i = 0; i < r->nhit; i++)
	{
		r->hit[i] = (SiftHit) {all[i].key, all[i].score};
		if (all[i].root != all[i].key)
			hot_remember(r, all[i].key, all[i].root);
	}
	rank_skip_units(r, sift_par_units_claimed(r->par, r->nunit), r->nunit);
	pfree(all);
}

#define RANK_INITIAL_K	1024

/* Searches the K best verified hits from scratch; the heap grows with the hits found, not with K (K is unbounded without a hint: one search holds every match) */
static void
rank_search(SiftRanker *r)
{
	SiftPlan   *p = r->plan;
	SiftTopK	tk;
	int64		cap;
	bool		shared = r->par && !r->searched;

	hot_forget(r);
	if (r->hit)
		pfree(r->hit);
	cap = Min(r->K, RANK_INITIAL_K);
	sift_kern_topk_init(&tk, MemoryContextAllocHuge(p->mcxt, cap * sizeof(SiftHit)), cap, r->K);
	p->stats.blocks_scored = p->stats.blocks_skipped = p->stats.candidates = p->stats.verified = 0;
	if (shared)
		rank_units_shared(r, &tk);
	else
		rank_units(r, &tk);
	p->stats.theta = sift_kern_topk_theta(&tk);
	r->complete = tk.n < r->K;
	r->nhit = sift_kern_topk_sort(&tk);
	r->hit = tk.hit;
	r->hitcap = tk.cap;
	r->searched = true;
	if (shared)
		rank_merge_shared(r);
}

/*
 * A consumer that pulls past K rows is lossy (a join or a filter above the scan): every search scores
 * the blocks again, so the next K grows fast while the hit array is small (an unfiltered join passes
 * about a tenth of the rows, a search of 160 serves it where 40 did not) and slowly beyond it.
 */
#define RANK_FAST_GROWTH_K	4096

int64
sift_rank_next_k(int64 k)
{
	return k * (k < RANK_FAST_GROWTH_K ? 16 : 4);
}

static void
rank_ensure(SiftRanker *r)
{
	if (!r->searched)
		r->K = r->k > 0 ? r->k : PG_INT64_MAX;
	else if (r->pos >= r->nhit && !r->complete)
		r->K = sift_rank_next_k(r->K);
	else
		return;
	rank_search(r);
}

bool
sift_rank_next(SiftRanker *r, ItemPointer tid, float4 *score)
{
	SiftKey		key;
	HotEnt	   *e;

	rank_ensure(r);
	if (r->pos >= r->nhit)
		return false;
	key = r->hit[r->pos].key;
	if (r->hot && (e = hash_search(r->hot, &key, HASH_FIND, NULL)) != NULL)
		key = e->root;
	sift_key_to_tid(key, tid);
	*score = r->hit[r->pos++].score;
	return true;
}

float4
sift_rank_max_score(SiftRanker *r)
{
	rank_ensure(r);
	return r->nhit > 0 ? r->hit[0].score : 0;
}
