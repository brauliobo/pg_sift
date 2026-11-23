/*
 * bm25/rank_search.c - the ranked search: best-first over units and blocks, verification and the top-k.
 */
#include "rank_int.h"
#include "core/memtrack.h"

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

void
hot_forget(SiftRanker *r)
{
	if (r->hot)
		hash_destroy(r->hot);
	r->hot = NULL;
}

/* The hits a search holds (the K best, the ties and the candidates waiting) must fit work_mem: K is unbounded without a limit, and one search then holds every match */
static void
rank_budget(const SiftTopK *tk, int64 hits)
{
	sift_budget_check((hits + tk->tiecap) * sizeof(SiftHit) + tk->lazycap * sizeof(SiftLazyHit), work_mem,
					  "a ranked search of the matches", "work_mem", "add a LIMIT that the search can take (a filter after the search keeps every match)");
}

/*
 * A search without a limit holds every match, which may be more than work_mem: it keeps its hits in memory up to half of it and sorts the rest outside, in a tuplesort
 * of the other half (bm25/hitspill.h). The hits come back from there best first, as from the heap.
 */
static bool
rank_spill_flush(SiftRanker *r, SiftTopK *tk)
{
	if (!r->spill_ok || ((int64) Min(tk->k, 2 * tk->cap) * sizeof(SiftHit) + tk->lazycap * sizeof(SiftLazyHit)) / 1024 <= work_mem / 2)
		return false;
	if (r->spill == NULL && (r->spill = sift_hitspill_begin(r->plan->mcxt, work_mem / 2)) == NULL)
	{
		r->spill_ok = false;
		return false;
	}
	for (int64 i = 0; i < tk->n; i++)
		sift_hitspill_put(r->spill, tk->hit[i].key, tk->hit[i].score);
	r->nhit += tk->n;
	tk->n = 0;
	return true;
}

/* The search is over: what the heap still holds joins the sorted hits, which are all of them */
static void
rank_spill_finish(SiftRanker *r, SiftTopK *tk)
{
	for (int64 i = 0; i < tk->n; i++)
		sift_hitspill_put(r->spill, tk->hit[i].key, tk->hit[i].score);
	r->nhit += tk->n;
	pfree(tk->hit);
	sift_hitspill_sort(r->spill);
	r->hit = NULL;
	r->hitcap = 0;
}

/* The hit at pos, read ahead from the sorted hits */
static const SiftHit *
rank_spill_head(SiftRanker *r)
{
	if (!r->spill_ready)
		r->spill_ready = sift_hitspill_next(r->spill, &r->spill_head.key, &r->spill_head.score);
	return &r->spill_head;
}

void
rank_spill_end(SiftRanker *r)
{
	if (r->spill)
		sift_hitspill_end(r->spill);
	r->spill = NULL;
	r->spill_ready = false;
}

/* A tie-inclusive search keeps, next to the K best hits, every verified hit that ties with the worst of them (r->ties) */
static void
tie_add(SiftRanker *r, SiftTopK *tk, SiftKey key, float4 score)
{
	if (tk->ntie == tk->tiecap)
	{
		tk->tiecap = Max(2 * tk->tiecap, 256);
		rank_budget(tk, tk->cap);
		SIFT_REGROW_IN(r->plan->mcxt, tk->tie, tk->tiecap * sizeof(SiftHit));
	}
	tk->tie[tk->ntie++] = (SiftHit) {.key = key,.score = score};
}

static void
tie_defer(SiftRanker *r, SiftTopK *tk, SiftKey key, float4 score, int seg)
{
	if (tk->nlazy == tk->lazycap)
	{
		tk->lazycap = Max(2 * tk->lazycap, 256);
		rank_budget(tk, tk->cap);
		SIFT_REGROW_IN(r->plan->mcxt, tk->lazy, tk->lazycap * sizeof(SiftLazyHit));
	}
	tk->lazy[tk->nlazy++] = (SiftLazyHit) {.key = key,.score = score,.seg = seg};
}

static inline bool
tie_hit(const SiftRanker *r, const SiftTopK *tk, float4 score)
{
	return r->ties && tk->n == tk->k && score == tk->hit[0].score;
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

	if (!sift_kern_topk_admits(tk, score, (key & ~UINT64CONST(0xFFFF)) | 1) && !tie_hit(r, tk, score))
		return;
	if (seg >= 0 && r->posok)
	{
		if (!phrases_hold(r->phrpos, seg, sift_key_block(key) & (SIFT_SB_PAGES - 1), (key & 0xFFFF) - 1))
			return;
		heap = p->recheck || p->segheap[seg];
	}
	/*
	 * A candidate that cannot enter the kept hits but ties with the worst of them is only a tie if the worst score stays: the early
	 * worst scores of a search are low and their groups large, so it waits (its score is exact when no text decides it) and the
	 * search verifies the ones that tie with the final worst score (rank_close_ties)
	 */
	if (!heap && r->ties && !sift_kern_topk_admits(tk, score, (key & ~UINT64CONST(0xFFFF)) | 1))
	{
		tie_defer(r, tk, key, score, seg);
		return;
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
	if (sift_kern_topk_admits(tk, score, vis))
	{
		bool		full = tk->n == tk->k;
		SiftHit		out = full ? tk->hit[0] : (SiftHit) {0};

		if (tk->n == tk->cap && tk->n < tk->k && !rank_spill_flush(r, tk))
		{
			rank_budget(tk, Min(tk->k, 2 * tk->cap));
			tk->hit = repalloc(tk->hit, (tk->cap = Min(tk->k, 2 * tk->cap)) * sizeof(SiftHit));
		}
		sift_kern_topk_push(tk, score, vis);
		/* the hit the new one pushed out ties with the new worst one, or the worst score rose and the ties are behind it */
		if (r->ties && full)
		{
			if (tk->hit[0].score == out.score)
				tie_add(r, tk, out.key, out.score);
			else
				tk->ntie = tk->nlazy = 0;
		}
	}
	else if (tie_hit(r, tk, score))
		tie_add(r, tk, vis, score);
	else
		return;
	if (vis != key)
		hot_remember(r, vis, key);
}

/* Verifies the deferred candidates that tie with the final worst score and keeps the ones that pass */
static void
rank_close_ties(SiftRanker *r, SiftTopK *tk)
{
	SiftPlan   *p = r->plan;

	for (int64 i = 0; i < tk->nlazy && tk->n == tk->k; i++)
	{
		SiftLazyHit *c = &tk->lazy[i];
		ItemPointerData tid;
		text	   *doc = NULL;
		MemoryContext old;
		bool		ok;
		SiftKey		vis;

		if (c->score != tk->hit[0].score)
			continue;
		CHECK_FOR_INTERRUPTS();
		sift_key_to_tid(c->key, &tid);
		p->stats.candidates++;
		old = MemoryContextSwitchTo(r->tmpcx);
		ok = r->verify(r->arg, &tid, false, &doc);
		MemoryContextSwitchTo(old);
		MemoryContextReset(r->tmpcx);
		if (!ok)
			continue;
		p->stats.verified++;
		vis = sift_key_from_tid(&tid);
		tie_add(r, tk, vis, c->score);
		if (vis != c->key)
			hot_remember(r, vis, c->key);
	}
	tk->nlazy = 0;
}

static int
hit_key_cmp(const void *a, const void *b)
{
	return pg_cmp_u64(((const SiftHit *) a)->key, ((const SiftHit *) b)->key);
}

/* The tied hits follow the sorted ones, by ascending key as equal scores sort */
static void
rank_append_ties(SiftTopK *tk)
{
	if (tk->ntie == 0)
		return;
	if (tk->n + tk->ntie > tk->cap)
	{
		rank_budget(tk, tk->n + tk->ntie);
		tk->hit = repalloc(tk->hit, (tk->cap = tk->n + tk->ntie) * sizeof(SiftHit));
	}
	qsort(tk->tie, tk->ntie, sizeof(SiftHit), hit_key_cmp);
	memcpy(tk->hit + tk->n, tk->tie, tk->ntie * sizeof(SiftHit));
	tk->n += tk->ntie;
	tk->ntie = 0;
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
	double		total = 0,
				slack = sum_ub_slack(p->nleaves);

	for (int i = 0; i < p->nleaves; i++)
		total += (double) rs->lmax[i];
	for (int i = 0; i < p->nleaves; i++)
	{
		RkTerm	   *t = rs->term + rs->first[i];
		sift_cur   *orc = rs->cs.leafor[i];
		int			nt = p->leaves[i].nterms[rs->s],
					nlive = 0;
		double		other = total - (double) rs->lmax[i];

		for (int j = 0; p->leaves[i].scoring && j < nt; j++)
		{
			t[j].g = NULL;
			t[j].based = false;
			t[j].located = t[j].sought = false;
			t[j].live = nt == 1 || (other + (double) t[j].tub) * slack > (double) theta;
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

	if (!t->located)
	{
		t->bb = term_bound_find(t, gid);
		t->located = true;
	}
	t->sought = true;
	if (t->bb == NULL)
		return;
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
	Assert(g && g->gid == gid);
	t->g = g;
}

/*
 * The terms that have postings in block b of the unit's SB look their group up when the block is first scored: a unit whose blocks are all
 * out of reach, and a term without postings in the blocks that are scored, cost no seek. A block bound of 0 is a block without postings.
 */
static void
block_seek(SiftPlan *p, RankSeg *rs, uint32 gid, int b)
{
	for (int i = 0; i < p->nleaves; i++)
		for (int j = 0; p->leaves[i].scoring && j < p->leaves[i].nterms[rs->s]; j++)
		{
			RkTerm	   *t = &rs->term[rs->first[i] + j];

			if (!t->live || t->sought)
				continue;
			if (!t->located)
			{
				t->bb = term_bound_find(t, gid);
				t->located = true;
			}
			if (t->bb && t->bb->blk_bound[b] > 0)
				term_seek(p, rs, t, gid);
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

#define ZONE_TRIES	32

static void
rank_unit(SiftRanker *r, const Unit *u, SiftTopK *tk)
{
	SiftPlan   *p = r->plan;
	RankSeg    *rs = &r->seg[u->seg];
	SiftSegment *seg = p->seg[u->seg];
	const sift_grp *g;
	const SiftFilterSB *fs;
	const SiftTidBridgeSB *bsb;
	SiftDocChunk dc;
	float4		theta = rank_theta(r, tk);
	int			order[SIFT_BLKS_PER_SB],
				n = 0,
				nk;
	uint64		pages[4];
	bool		have;

	/* data that is not ordered like the column has no superblock to drop: stop asking when none of the first ones was */
	if (p->nattrs && (r->wk->zone_drops > 0 || r->wk->zone_checks < ZONE_TRIES))
	{
		r->wk->zone_checks++;
		if (sift_seg_attr_excludes(seg, u->gid, p->attrs, p->nattrs))
		{
			r->wk->zone_drops++;
			p->stats.blocks_skipped += u->nblk;
			sift_sr_release(&seg->sr);
			return;
		}
	}
	if ((int64) u->gid < rs->last)
		seg_rewind(rs);
	rs->last = u->gid;
	unit_terms(p, rs, theta);
	for (ListCell *lc = list_head(rs->cs.ops); lc; lc = lnext(rs->cs.ops, lc))
		((sift_cur *) lfirst(lc))->state = 0;
	/* an OR root is not merged into one group of the whole superblock: its kids' groups are merged per block, for the blocks that are scored */
	if (rs->nug > 0)
	{
		sift_cur  **kid = sift_cur_or_kids(rs->root, &nk);

		memset(pages, 0, sizeof(pages));
		for (int i = 0; i < nk; i++)
		{
			rs->ug[i] = sift_seek_eq(kid[i], u->gid);
			for (int w = 0; rs->ug[i] && w < 4; w++)
				pages[w] |= rs->ug[i]->pages[w];
		}
		g = NULL;
		have = (pages[0] | pages[1] | pages[2] | pages[3]) != 0;
	}
	else
	{
		g = sift_seek_eq(rs->root, u->gid);
		have = g != NULL;
		if (have)
			memcpy(pages, g->pages, sizeof(pages));
	}
	p->stats.sb_scanned++;
	bsb = p->bridge ? sift_tidbridge_probe_sb(p->bridge, u->gid) : NULL;
	Assert(bsb || !p->bridge);
	/* no page of the segment's group is one the bridge holds: the docmap of the superblock is never read */
	if (have && bsb && !(have = sift_tidbridge_intersect_pages(bsb, pages, pages)))
	{
#ifdef SIFT_TESTING
		sift_rank_stats.bridge_docmap_skips++;
#endif
	}
	if (!have || !sift_seg_docs(seg, u->gid, &dc))
	{
		sift_sr_release(&seg->sr);
		return;
	}
	fs = p->filter ? sift_filter_sb(p->filter, u->gid) : NULL;
	Assert(fs || !p->filter);
	for (int b = 0; b < SIFT_BLKS_PER_SB; b++)
		if (pages[b / 4] >> ((b % 4) * 16) & 0xffff && (fs == NULL || sift_filter_has_block(fs, b)))
			order[n++] = b;
	if (n == 0)
	{
		sift_sr_release(&seg->sr);
		return;
	}
	for (int k = 0; k < p->npleaf; k++)
		for (int j = 0; j < p->leaves[p->pleaf[k]].nterms[u->seg]; j++)
			if (rs->term[rs->first[p->pleaf[k]] + j].live && !rs->term[rs->first[p->pleaf[k]] + j].sought)
				term_seek(p, rs, &rs->term[rs->first[p->pleaf[k]] + j], u->gid);
	r->posok = p->nphr > 0 && rank_phrase_groups(r, rs);
	order_blocks(order, n, u->ub);
	for (int i = 0; i < n; i++)
	{
		int			b = order[i],
					nsel;
		uint32		live;

		theta = rank_theta(r, tk);
		if (!(u->ub[b] > theta))
		{
			p->stats.blocks_skipped++;
			continue;
		}
		block_seek(p, rs, u->gid, b);
		if (!block_terms(p, rs, b, theta, r->leafub))
		{
			p->stats.blocks_skipped++;
			continue;
		}
		live = theta > -INFINITY ? block_live(p, rs, b, theta) : 0xffff;
		if (!live)
		{
			p->stats.blocks_skipped++;
			continue;
		}
		p->stats.blocks_scored++;
		nsel = score_block(r, rs, g, fs, bsb, &dc, b, theta, r->leafub, live);
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
		if ((p->filter == NULL || sift_filter_has(p->filter, p->mhit[i].key)) &&
			(p->bridge == NULL || sift_tidbridge_probe_tid(p->bridge, p->mhit[i].key, NULL) != SIFT_TIDPROBE_NO))
			rank_candidate(r, tk, p->mhit[i].key, p->mhit[i].score, -1);
}

/*
 * A plan with a complement branch has matches that no leaf hits, so no bound finds them: every match of the iterator is a candidate. One
 * that no leaf scores scores 0 (the search ends once k are kept, as keys ascend); else its score comes from its text (rank_candidate
 * rescores), so the bound it is admitted with is infinite.
 */
static void
rank_zero(SiftRanker *r, SiftTopK *tk)
{
	SiftPlan   *p = r->plan;
	SiftMatchIter *it = sift_iter_begin(p);
	SiftPageMatch pm;
	float4		bound = p->rescore ? INFINITY : 0;

	while (sift_iter_next(it, &pm))
	{
		for (int w = 0; w < SIFT_OFFMASK_WORDS; w++)
			for (uint64 m = pm.offmask[w]; m; m &= m - 1)
			{
				SiftKey		key = (uint64) pm.blk << 16 | (w * 64 + pg_rightmost_one_pos64(m) + 1);

				CHECK_FOR_INTERRUPTS();
				if ((p->filter == NULL || sift_filter_has(p->filter, key)) &&
					(p->bridge == NULL || sift_tidbridge_probe_tid(p->bridge, key, NULL) != SIFT_TIDPROBE_NO))
					rank_candidate(r, tk, key, bound, -1);
			}
		if (!p->rescore && !r->ties && tk->n == tk->k)
			break;
	}
	sift_iter_end(it);
}

#define RANK_FIRST_BATCH	4
#define RANK_BATCH			16

static inline bool
unit_before(const Unit *a, const Unit *b)
{
	return a->seg != b->seg ? a->seg < b->seg : a->gid < b->gid;
}

/*
 * BATCH_BMW: the units by descending bound, a batch at a time (the first one small, so the threshold rises at once); a batch runs in
 * ascending (segment, gid) order, which the cursors follow forward where the order of the bounds sends them back and forth.
 */
static void
rank_units_batched(SiftRanker *r, SiftTopK *tk)
{
	int			u = 0,
				size = RANK_FIRST_BATCH;

	rank_sort_units(r);
	while (u < r->nunit && r->unit[u].maxub > sift_kern_topk_theta(tk))
	{
		const Unit *batch[RANK_BATCH];
		int			n = 0,
					end = Min(u + size, r->nunit);

		for (; u < end; u++)
			if (r->unit[u].maxub > sift_kern_topk_theta(tk))
			{
				int			j = n++;

				for (; j > 0 && unit_before(&r->unit[u], batch[j - 1]); j--)
					batch[j] = batch[j - 1];
				batch[j] = &r->unit[u];
			}
			else
				r->plan->stats.blocks_skipped += r->unit[u].nblk;
		for (int i = 0; i < n; i++)
		{
			CHECK_FOR_INTERRUPTS();
			if (batch[i]->maxub > sift_kern_topk_theta(tk))
				rank_unit(r, batch[i], tk);
			else
				r->plan->stats.blocks_skipped += batch[i]->nblk;
		}
		size = RANK_BATCH;
	}
	rank_skip_units(r, u, r->nunit);
}

/*
 * MAXSCORE_SB and CONJUNCTIVE: the units are made as they are walked, in ascending (segment, gid) order (rank_stream.c), none is kept. The best
 * few go first, so that the threshold is near its final value before the sweep; the sweep then takes every other unit in order, and a unit
 * whose bound is out of reach costs nothing. No unit is ordered by bound and the cursors never go back, except once at the start of the sweep.
 */
static void
rank_units_streamed(SiftRanker *r, SiftTopK *tk)
{
	Unit		seed[RANK_SEED],
				u;
	int			nseed = r->k > 0 ? rank_seed_units(r, seed) : 0;

	Assert(!r->by_maxub);
	for (int i = 0; i < nseed; i++)
	{
		CHECK_FOR_INTERRUPTS();
		if (seed[i].maxub > sift_kern_topk_theta(tk))
			rank_unit(r, &seed[i], tk);
		else
			r->plan->stats.blocks_skipped += seed[i].nblk;
	}
	for (int s = 0; s < r->plan->nsegs; s++)
	{
		UnitStream *st = unit_stream_open(r, s, seed, nseed);

		while (st && unit_stream_next(st, tk, &u))
			rank_unit(r, &u, tk);
	}
}

static void
rank_units(SiftRanker *r, SiftTopK *tk)
{
	if (r->plan->complement)
	{
		rank_zero(r, tk);
		return;
	}
	rank_wbuf(r, tk);
	if (r->mode == SIFT_RM_BATCH_BMW || r->by_maxub)
		rank_units_batched(r, tk);
	else
		rank_units_streamed(r, tk);
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
	{
		if (r->plan->complement)
			rank_zero(r, tk);
		else
			rank_wbuf(r, tk);
	}
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
	/* the hits that tie with the K-th one are all there: each participant published its own */
	while (r->ties && r->nhit < n && all[r->nhit].score == all[r->K - 1].score)
		r->nhit++;
	r->complete = r->ties || n < r->K;
	if (r->nhit > r->hitcap)
		r->hit = repalloc(r->hit, (r->hitcap = r->nhit) * sizeof(SiftHit));
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
rank_search_once(SiftRanker *r)
{
	SiftPlan   *p = r->plan;
	SiftTopK	tk;
	int64		cap;
	bool		shared = r->par && !r->searched;

	hot_forget(r);
	rank_spill_end(r);
	if (r->hit)
		pfree(r->hit);
	r->spill_ok = r->K == PG_INT64_MAX && !r->ties && !shared;
	r->nhit = 0;
	cap = Min(r->K, RANK_INITIAL_K);
	sift_kern_topk_init(&tk, MemoryContextAlloc(p->mcxt, cap * sizeof(SiftHit)), cap, r->K);
	tk.floor = r->group_floor && !r->searched ? Max(r->floor, *r->group_floor) : r->floor;
#ifdef SIFT_TESTING
	sift_rank_stats.floor_searches += r->group_floor && !r->searched && *r->group_floor > -INFINITY;
#endif
	p->stats.blocks_scored = p->stats.blocks_skipped = p->stats.candidates = p->stats.verified = 0;
	if (shared)
		rank_units_shared(r, &tk);
	else
		rank_units(r, &tk);
	p->stats.theta = sift_kern_topk_theta(&tk);
	if (r->group_floor && !r->searched && tk.n == tk.k)
		*r->group_floor = Max(*r->group_floor, p->stats.theta);
	r->complete = r->ties || tk.n < r->K;
	rank_close_ties(r, &tk);
	if (r->spill)
		rank_spill_finish(r, &tk);
	else
	{
		sift_kern_topk_sort(&tk);
		rank_append_ties(&tk);
		r->nhit = tk.n;
		r->hit = tk.hit;
		r->hitcap = tk.cap;
	}
	r->searched = true;
	if (shared)
		rank_merge_shared(r);
}

/*
 * A plan with deferred wide leaves is searched on their top terms first: when the K-th score of that search (every result of it scores as much
 * with every term read) is out of reach of the terms left out, its results are the plan's, and the leaves' postings were never unioned. When it is
 * not, the leaves read the terms that can reach it and the plan is searched again; a later search of more hits has a lower K-th score and may widen it more.
 */
static void
rank_search(SiftRanker *r)
{
	r->plan->stats.searches++;
	rank_search_once(r);
	if (r->deferred && sift_plan_tail_settled(r->plan, r->plan->stats.theta))
	{
#ifdef SIFT_TESTING
		sift_rank_stats.tail_settled++;
#endif
	}
	else if (r->deferred)
	{
#ifdef SIFT_TESTING
		sift_rank_stats.tail_widened++;
#endif
		rank_widen(r, r->plan->stats.theta);
		r->floor = r->par || r->plan->rescore ? -INFINITY : r->plan->stats.theta;
		rank_search_once(r);
		r->floor = -INFINITY;
	}
	if (r->K == r->k && !r->par && !r->group_floor && !r->plan->complement)
		sift_plan_remember_theta(r->plan, r->K, r->plan->stats.theta);
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

/*
 * The hits that the rows the consumer still wants take, at the rate it kept in the hits the last search added (the first search: of all),
 * three standard deviations over, and at least 4 times the last search: the kept rows of a score are seldom spread as evenly as the
 * rows are, and each search scores the blocks again.
 */
static int64
rank_dropped_k(SiftRanker *r)
{
	double		added = Max(r->fb_read - r->fb_read0, 1),
				rate = Max(r->fb_kept - r->fb_kept0, 0.5) / added,
				need = Max(r->fb_want, 1) / rate;

	r->fb_read0 = r->fb_read;
	r->fb_kept0 = r->fb_kept;
	return Max(r->K * 4, (int64) ceil(r->fb_read + need + 3 * sqrt(need)));
}

static void
rank_ensure(SiftRanker *r)
{
	if (!r->searched)
		r->K = r->k > 0 ? r->k : PG_INT64_MAX;
	else if (r->pos >= r->nhit && !r->complete)
		r->K = r->dropping ? rank_dropped_k(r) : sift_rank_next_k(r->K);
	else
		return;
	rank_search(r);
}

void
sift_rank_ties(SiftRanker *r)
{
	Assert(r->k > 0 && !r->searched);
	r->ties = true;
}

void
sift_rank_group_floor(SiftRanker *r, float4 *cell)
{
	Assert(!r->searched);
	r->group_floor = cell;
}

void
sift_rank_reopen(SiftRanker *r)
{
	if (!r->ties)
		return;
	r->ties = false;
	r->complete = false;
	r->K = Max(r->K, r->nhit);
}

void
sift_rank_feedback(SiftRanker *r, int64 read, int64 kept, int64 want)
{
	r->dropping = true;
	r->fb_read = read;
	r->fb_kept = kept;
	r->fb_want = want;
}

bool
sift_rank_next(SiftRanker *r, ItemPointer tid, float4 *score)
{
	SiftKey		key;
	HotEnt	   *e;
	const SiftHit *hit;

	rank_ensure(r);
	if (r->pos >= r->nhit)
		return false;
	hit = r->spill ? rank_spill_head(r) : &r->hit[r->pos];
	key = hit->key;
	if (r->hot && (e = hash_search(r->hot, &key, HASH_FIND, NULL)) != NULL)
		key = e->root;
	sift_key_to_tid(key, tid);
	*score = hit->score;
	r->pos++;
	r->spill_ready = false;
	return true;
}

float4
sift_rank_max_score(SiftRanker *r)
{
	rank_ensure(r);
	if (r->nhit == 0)
		return 0;
	return r->spill ? rank_spill_head(r)->score : r->hit[0].score;
}
