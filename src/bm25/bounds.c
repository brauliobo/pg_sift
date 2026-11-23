/*
 * bm25/bounds.c - opens the ranker: the score bounds of the leaves per superblock and the units in descending bound order.
 */
#include "core/host.h"

#include <math.h>

#include "rank_int.h"
#include "bm25/kernels.h"
#include "core/cmp.h"
#include "core/memtrack.h"

#ifdef SIFT_TESTING
int			sift_test_rank_mode = -1;
SiftRankStats sift_rank_stats;
#endif

/* The cursor a leaf is found by in the kids of the root OR: found by a scan up to this many, else by a bisection of them sorted by pointer */
#define LEAF_MAP_SCAN	16

typedef struct LeafCur
{
	const sift_cur *cur;
	int			leaf;
} LeafCur;

static int
leafcur_cmp(const void *a, const void *b)
{
	const LeafCur *x = a,
			   *y = b;

	if (x->cur != y->cur)
		return (uintptr_t) x->cur < (uintptr_t) y->cur ? -1 : 1;
	return x->leaf < y->leaf ? -1 : x->leaf > y->leaf;
}

static int
gid_ub_cmp(const void *a, const void *b)
{
	uint32		x = ((const GidUB *) a)->gid,
				y = ((const GidUB *) b)->gid;

	return x < y ? -1 : x > y;
}

/* The bounds of one leaf in segment s: the max over its terms per SB */
static void
leaf_bounds(SiftPlan *p, RankSeg *rs, int leaf, MemoryContext cx)
{
	const EvalLeaf *l = &p->leaves[leaf];
	LeafUB	   *out = &rs->lub[leaf];
	int			nt = l->nterms[rs->s],
				total = 0;
	GidUB	   *all;
	double		ref = p->seg[rs->s]->hdr->avgdl_ref;

	for (int i = 0; i < nt; i++)
	{
		int			n;

		sift_post_bounds(rs->cs.leafpc[leaf][i], &n);
		total += n;
	}
	/* MEM[MEM-BM25-11]: bounds/rank_search scratch (2.1x leaf_bounds + 2x tie append + undercounted spill) */
	/* check-memory: allow total counts the superblocks of the leaf's terms in one segment: at most nterms * nsb entries */
	all = MemoryContextAlloc(cx, Max(total, 1) * sizeof(GidUB));
	total = 0;
	for (int i = 0; i < nt; i++)
	{
		int			n;
		const SiftGroupBounds *gb = sift_post_bounds(rs->cs.leafpc[leaf][i], &n);
		float4		w = sift_kern_ub_weight(l->terms[rs->s][i].w, p->stats.avgdl, ref, p->nleaves);

		for (int j = 0; j < n; j++, total++)
		{
			all[total].gid = gb[j].gid;
			memset(all[total].ub, 0, sizeof(all[total].ub));
			sift_kern->ub_accum(all[total].ub, gb[j].blk_bound, w, SIFT_BLKS_PER_SB);
		}
	}
	if (nt > 1)
		qsort(all, total, sizeof(GidUB), gid_ub_cmp);
	for (int i = 0; i < total; i++)
	{
		if (out->n > 0 && all[out->n - 1].gid == all[i].gid)
			for (int k = 0; k < SIFT_BLKS_PER_SB; k++)
				all[out->n - 1].ub[k] = Max(all[out->n - 1].ub[k], all[i].ub[k]);
		else
			all[out->n++] = all[i];
	}
	out->list = all;
}

/* Does the bounded tree have a possible match in the SB whose leaf bounds are ubp (NULL: no posting of the leaf there)? Leaves that do not score have none: they may match anywhere */
static bool
tree_present(const SiftPlan *p, const EvalNode *n, const float4 *const *ubp)
{
	switch (n->kind)
	{
		case EN_LEAF:
			return !p->leaves[n->leaf].scoring || ubp[n->leaf] != NULL;
		case EN_AND:
			for (int i = 0; i < n->nchild; i++)
				if (n->child[i]->kind != EN_NOT && !tree_present(p, n->child[i], ubp))
					return false;
			return true;
		case EN_OR:
			for (int i = 0; i < n->nchild; i++)
				if (tree_present(p, n->child[i], ubp))
					return true;
			return false;
		default:
			return false;
	}
}

/* Has the bridge a page in scoring block b of the superblock */
static inline bool
bridge_has_block(const SiftTidBridgeSB *bsb, int b)
{
	return (bsb->page_mask[b / 4] >> ((b % 4) * SIFT_BLK_PAGES) & 0xffff) != 0;
}

/* The unit of superblock gid of the segment, when the filter and the bridge keep a document of it and the tree can match there; ubp[leaf] is the leaf's bounds there, NULL for none */
bool
unit_make(const SiftRanker *r, int s, uint32 gid, const float4 *const *ubp, Unit *out)
{
	const SiftPlan *p = r->plan;
	Unit		u = {.seg = s,.gid = gid};
	const SiftFilterSB *fs = p->filter ? sift_filter_sb(p->filter, gid) : NULL;
	const SiftTidBridgeSB *bsb = p->bridge ? sift_tidbridge_probe_sb(p->bridge, gid) : NULL;

	if ((p->filter && !fs) || (p->bridge && !bsb) || !tree_present(p, p->root, ubp))
		return false;
	for (int i = 0; i < p->nleaves; i++)
		if (ubp[i])
			for (int k = 0; k < SIFT_BLKS_PER_SB; k++)
				u.ub[k] += ubp[i][k];
	for (int k = 0; k < SIFT_BLKS_PER_SB; k++)
	{
		/* a block without a page the filter or the bridge keeps scores nothing */
		if ((fs && !sift_filter_has_block(fs, k)) || (bsb && !bridge_has_block(bsb, k)))
			u.ub[k] = 0;
		u.maxub = Max(u.maxub, u.ub[k]);
		u.nblk += u.ub[k] > 0;
	}
	*out = u;
	return true;
}

static void
unit_add(SiftRanker *r, int s, uint32 gid, const float4 *const *ubp, Unit **units, int *nunits, int *cap)
{
	Unit		u;

	if (!unit_make(r, s, gid, ubp, &u))
		return;
	if (*nunits == *cap)
	{
		*cap = Max(64, 2 * *cap);
		SIFT_REGROW_IN(r->plan->mcxt, *units, *cap * sizeof(Unit));
	}
	(*units)[(*nunits)++] = u;
}

/* The term's bounds of superblock gid, NULL when the term has no posting there */
const SiftGroupBounds *
term_bound_find(const RkTerm *t, uint32 gid)
{
	int			lo = 0,
				hi = t->ngb;

	SIFT_BISECT(int, mid, lo, hi, t->gb[mid].gid < gid);
	return lo < t->ngb && t->gb[lo].gid == gid ? &t->gb[lo] : NULL;
}

/* The bound of a leaf in superblock gid from its terms' own lists (the max over its terms, as leaf_bounds merges them); false when no term has postings there */
bool
leaf_bound_at(const SiftPlan *p, const RankSeg *rs, int leaf, uint32 gid, float4 *ub)
{
	const RkTerm *t = rs->term + rs->first[leaf];
	bool		any = false;

	memset(ub, 0, SIFT_BLKS_PER_SB * sizeof(float4));
	for (int j = 0; j < p->leaves[leaf].nterms[rs->s]; j++)
	{
		const SiftGroupBounds *gb = term_bound_find(&t[j], gid);
		float4		v[SIFT_BLKS_PER_SB] = {0};

		if (gb == NULL)
			continue;
		any = true;
		sift_kern->ub_accum(v, gb->blk_bound, t[j].ubw, SIFT_BLKS_PER_SB);
		for (int k = 0; k < SIFT_BLKS_PER_SB; k++)
			ub[k] = Max(ub[k], v[k]);
	}
	return any;
}

/* The first superblock entry of the bridge at or after sb */
uint32
bridge_first_sb(const SiftTidBridge *br, uint32 sb)
{
	uint32		lo = 0,
				hi = br->nsbs;

	while (lo < hi)
	{
		uint32		mid = lo + (hi - lo) / 2;

		if (br->sbs[mid].sb < sb)
			lo = mid + 1;
		else
			hi = mid;
	}
	return lo;
}

/* Units of one segment, ascending gid: the superblocks of the bridge, the bounds of the leaves read from the terms' lists there */
static void
seg_units_bridge(SiftRanker *r, RankSeg *rs, Unit **units, int *nunits, int *cap)
{
	SiftPlan   *p = r->plan;
	const SiftTidBridge *br = p->bridge;
	const SiftSegHeaderData *h = p->seg[rs->s]->hdr;
	const float4 **ubp = palloc(Max(p->nleaves, 1) * sizeof(float4 *));
	float4	  (*buf)[SIFT_BLKS_PER_SB] = palloc(Max(p->nleaves, 1) * sizeof(*buf));

	for (uint32 i = bridge_first_sb(br, h->sb_lo); i < br->nsbs && br->sbs[i].sb <= h->sb_hi; i++)
	{
		for (int l = 0; l < p->nleaves; l++)
			ubp[l] = p->leaves[l].scoring && leaf_bound_at(p, rs, l, br->sbs[i].sb, buf[l]) ? buf[l] : NULL;
		unit_add(r, rs->s, br->sbs[i].sb, ubp, units, nunits, cap);
	}
	pfree(ubp);
	pfree(buf);
}

/* Units of one segment, ascending gid: the leaf lists merged (a heap of them by the gid they stand on) and the tree evaluated per SB */
static void
seg_units(SiftRanker *r, RankSeg *rs, Unit **units, int *nunits, int *cap)
{
	SiftPlan   *p = r->plan;
	int			nl = Max(p->nleaves, 1),
				nh = 0;
	const float4 **ubp;
	GidEnt	   *heap;
	int		   *hit;

	if (rs->inverted)
		return seg_units_bridge(r, rs, units, nunits, cap);
	ubp = palloc0(nl * sizeof(float4 *));
	heap = palloc(nl * sizeof(GidEnt));
	hit = palloc(nl * sizeof(int));
	for (int i = 0; i < p->nleaves; i++)
		if (rs->lub[i].pos < rs->lub[i].n)
			heap[nh++] = (GidEnt) {rs->lub[i].list[rs->lub[i].pos].gid, i};
	gidheap_build(heap, nh);
	while (nh > 0)
	{
		uint32		gid = heap[0].gid;
		int			nhit = 0;

		for (; nh > 0 && heap[0].gid == gid; nhit++)
		{
			LeafUB	   *l = &rs->lub[hit[nhit] = heap[0].idx];

			ubp[hit[nhit]] = l->list[l->pos].ub;
			gidheap_top_to(heap, &nh, UINT32_MAX);
		}
		unit_add(r, rs->s, gid, ubp, units, nunits, cap);
		for (int k = 0; k < nhit; k++)
		{
			LeafUB	   *l = &rs->lub[hit[k]];

			ubp[hit[k]] = NULL;
			if (++l->pos < l->n)
			{
				gidheap_push(heap, &nh, (GidEnt) {l->list[l->pos].gid, hit[k]});
			}
		}
	}
	pfree(ubp);
	pfree(heap);
	pfree(hit);
}

static int
unit_cmp(const void *a, const void *b)
{
	const Unit *x = a,
			   *y = b;

	if (x->maxub != y->maxub)
		return x->maxub > y->maxub ? -1 : 1;
	if (x->gid != y->gid)
		return x->gid < y->gid ? -1 : 1;
	return x->seg - y->seg;
}

/* The units by descending bound, where a search that prunes by bound (or hands them out to workers) takes them */
void
rank_sort_units(SiftRanker *r)
{
	if (!r->by_maxub)
		qsort(r->unit, r->nunit, sizeof(Unit), unit_cmp);
	r->by_maxub = true;
}

#define RANK_BATCH_MAX_LEAVES	4
#define RANK_BATCH_MAX_UNITS	2048
#define RANK_SKEW				2.5f

/* The superblocks the leaf's terms have postings in, summed over its terms and segments: the units it can make, with overlaps counted twice */
static uint64
leaf_sbs(const SiftRanker *r, int leaf)
{
	uint64		n = 0;

	for (int s = 0; s < r->plan->nsegs; s++)
		if (r->seg[s].root)
			for (int j = 0; j < r->plan->leaves[leaf].nterms[s]; j++)
				n += r->seg[s].term[r->seg[s].first[leaf] + j].ngb;
	return n;
}

/* The largest bound of the heaviest leaf over that of the lightest: the leaves of a skewed query (a stop word and a rare one) are pruned best term by term */
static float4
leaf_skew(const SiftRanker *r)
{
	const SiftPlan *p = r->plan;
	float4		hi = 0,
				lo = INFINITY;

	for (int i = 0; i < p->nleaves; i++)
	{
		float4		gmax = 0;

		if (!p->leaves[i].scoring)
			continue;
		for (int s = 0; s < p->nsegs; s++)
			if (r->seg[s].root)
				gmax = Max(gmax, r->seg[s].lmax[i]);
		hi = Max(hi, gmax);
		lo = Min(lo, gmax);
	}
	return hi / Max(lo, 1e-6f);
}

/*
 * The walk a search takes over its units: without a LIMIT the threshold never rises, so ordering by bound prunes nothing and only makes
 * the cursors jump back; a root AND is aligned before it is decoded; a query of many, skewed or many-unit leaves is swept in order after
 * a seed; the rest (few balanced leaves, few units) takes the best units first, in batches that run in order.
 */
static SiftRankMode
rank_choose_mode_fast(const SiftRanker *r)
{
	const SiftPlan *p = r->plan;
	int			nscoring = 0,
				nrequired = 0;
	uint64		sum = 0,
				required_min = PG_UINT64_MAX;

#ifdef SIFT_TESTING
	if (sift_test_rank_mode >= 0)
		return (SiftRankMode) sift_test_rank_mode;
#endif
	for (int i = 0; i < p->nleaves; i++)
		if (p->leaves[i].scoring)
		{
			nscoring++;
			sum += leaf_sbs(r, i);
		}
	if (p->root->kind == EN_AND)
		for (int i = 0; i < p->root->nchild; i++)
			if (p->root->child[i]->kind == EN_LEAF && p->leaves[p->root->child[i]->leaf].scoring)
			{
				nrequired++;
				required_min = Min(required_min, leaf_sbs(r, p->root->child[i]->leaf));
			}
	if (r->k <= 0)
		return p->root->kind == EN_AND ? SIFT_RM_CONJUNCTIVE : SIFT_RM_MAXSCORE_SB;
	if (nrequired >= 2)
		return SIFT_RM_CONJUNCTIVE;
	if (nscoring > RANK_BATCH_MAX_LEAVES || (nrequired == 1 ? required_min : sum) > RANK_BATCH_MAX_UNITS ||
		(nscoring >= 3 && leaf_skew(r) >= RANK_SKEW))
		return SIFT_RM_MAXSCORE_SB;
	return SIFT_RM_BATCH_BMW;
}

/* Per-term bound weights of a scoring leaf, and the largest ub of the leaf */
static void
term_bounds(SiftPlan *p, RankSeg *rs, int leaf)
{
	double		ref = p->seg[rs->s]->hdr->avgdl_ref;

	for (int j = 0; j < p->leaves[leaf].nterms[rs->s]; j++)
	{
		RkTerm	   *t = &rs->term[rs->first[leaf] + j];
		t->gb = sift_post_bounds(t->pc, &t->ngb);
		uint8		top = t->ngb > 0 ? sift_post_max_bound(t->pc) : 0;

		t->ubw = sift_kern_ub_weight(t->w, p->stats.avgdl, ref, p->nleaves);
		t->tub = t->ubw * (float4) top;
		rs->lmax[leaf] = Max(rs->lmax[leaf], t->tub);
	}
}

/* The superblocks the bridge has in the segment are fewer than half of those the leaves' terms do: the bridge drives, no leaf list is merged */
static bool
bridge_drives(const SiftPlan *p, const RankSeg *rs)
{
	const SiftSegHeaderData *h = p->seg[rs->s]->hdr;
	uint64		driver = 0;

	if (p->bridge == NULL)
		return false;
	for (int i = 0; i < p->nleaves; i++)
		for (int j = 0; p->leaves[i].scoring && j < p->leaves[i].nterms[rs->s]; j++)
			driver += rs->term[rs->first[i] + j].ngb;
	return (uint64) (bridge_first_sb(p->bridge, h->sb_hi + 1) - bridge_first_sb(p->bridge, h->sb_lo)) * 2 < driver;
}

static void
rank_open_seg(SiftRanker *r, int s)
{
	SiftPlan   *p = r->plan;
	RankSeg    *rs = &r->seg[s];
	int			nterms = 0;

	rs->s = s;
	rs->last = -1;
	rs->cs.leafpc = MemoryContextAllocZero(p->mcxt, Max(p->nleaves, 1) * sizeof(SiftPostingCursor **));
	rs->cs.leafor = MemoryContextAllocZero(p->mcxt, Max(p->nleaves, 1) * sizeof(sift_cur *));
	rs->root = seg_cursor(p, s, p->root, &rs->cs, NULL, p->mcxt);
	if (rs->root == NULL)
		return;
	if (sift_cur_or_kids(rs->root, &rs->nug) != NULL)
		rs->ug = MemoryContextAllocZero(p->mcxt, rs->nug * sizeof(sift_grp *));
	else
		rs->nug = 0;
	rs->lmax = MemoryContextAllocZero(p->mcxt, Max(p->nleaves, 1) * sizeof(float4));
	rs->first = MemoryContextAlloc(p->mcxt, Max(p->nleaves, 1) * sizeof(int));
	for (int i = 0; i < p->nleaves; i++)
	{
		rs->first[i] = nterms;
		nterms += p->leaves[i].scoring ? p->leaves[i].nterms[s] : 0;
	}
	rs->nterm = nterms;
	rs->term = MemoryContextAllocZero(p->mcxt, Max(nterms, 1) * sizeof(RkTerm));
	for (int i = 0; i < p->nleaves; i++)
		if (p->leaves[i].scoring)
		{
			for (int j = 0; j < p->leaves[i].nterms[s]; j++)
			{
				rs->term[rs->first[i] + j].pc = rs->cs.leafpc[i][j];
				rs->term[rs->first[i] + j].w = p->leaves[i].terms[s][j].w;
			}
			term_bounds(p, rs, i);
		}
	rs->inverted = bridge_drives(p, rs);
#ifdef SIFT_TESTING
	sift_rank_stats.inverted_segs += rs->inverted;
#endif
	if (rs->nug > 0)
	{
		int			nk;
		sift_cur  **kid = sift_cur_or_kids(rs->root, &nk);

		rs->kidleaf = MemoryContextAlloc(p->mcxt, rs->nug * sizeof(int));
		rs->kidskip = MemoryContextAllocZero(p->mcxt, rs->nug * sizeof(bool));
		LeafCur    *lcs = palloc(Max(p->nleaves, 1) * sizeof(LeafCur));
		int			nlc = 0;

		for (int i = 0; i < p->nleaves; i++)
		{
			const EvalLeaf *l = &p->leaves[i];
			sift_cur   *lc = rs->cs.leafor[i] ? rs->cs.leafor[i] :
			l->scoring && l->nterms[s] == 1 ? sift_post_cursor(rs->cs.leafpc[i][0]) : NULL;

			if (lc != NULL)
				lcs[nlc++] = (LeafCur) {lc, i};
		}
		if (nlc > LEAF_MAP_SCAN && nk > LEAF_MAP_SCAN)
			qsort(lcs, nlc, sizeof(LeafCur), leafcur_cmp);
		for (int k = 0; k < nk; k++)
		{
			int			lo = 0,
						hi = nlc;

			rs->kidleaf[k] = -1;
			if (nlc <= LEAF_MAP_SCAN || nk <= LEAF_MAP_SCAN)
			{
				for (int i = 0; i < nlc; i++)
					if (lcs[i].cur == kid[k])
						rs->kidleaf[k] = lcs[i].leaf;
				continue;
			}
			SIFT_BISECT(int, mid, lo, hi, (uintptr_t) lcs[mid].cur <= (uintptr_t) kid[k]);
			if (lo > 0 && lcs[lo - 1].cur == kid[k])
				rs->kidleaf[k] = lcs[lo - 1].leaf;
		}
		pfree(lcs);
		rs->leaf_kid = MemoryContextAllocZero(p->mcxt, Max(p->nleaves, 1) * sizeof(bool));
		rs->ordpos = MemoryContextAlloc(p->mcxt, Max(p->nleaves, 1) * sizeof(int));
		for (int i = 0; i < p->nleaves; i++)
			rs->ordpos[i] = -1;
		for (int k = 0; k < nk; k++)
			if (rs->kidleaf[k] >= 0)
				rs->leaf_kid[rs->kidleaf[k]] = true;
	}
}

/* Every unit of every segment, ascending (segment, gid): what a search that orders or hands out its units walks; a streamed one makes them as it goes (rank_stream.c) */
static void
rank_materialize_units(SiftRanker *r)
{
	SiftPlan   *p = r->plan;
	MemoryContext cx = sift_mem_allocset_create(CurrentMemoryContext, "sift bounds");
	int			cap = 0;

	for (int s = 0; s < p->nsegs; s++)
	{
		RankSeg    *rs = &r->seg[s];

		if (rs->root == NULL)
			continue;
		rs->lub = MemoryContextAllocZero(cx, Max(p->nleaves, 1) * sizeof(LeafUB));
		for (int i = 0; i < p->nleaves && !rs->inverted; i++)
			if (p->leaves[i].scoring)
				leaf_bounds(p, rs, i, cx);
		seg_units(r, rs, &r->unit, &r->nunit, &cap);
		sift_sr_release(&p->seg[s]->sr);
	}
	MemoryContextDelete(cx);
	r->units_built = true;
}

/* The segments' terms and bounds, and the walk over their units: made again once the plan is widened */
static void
rank_open(SiftRanker *r)
{
	SiftPlan   *plan = r->plan;

	memset(r->seg, 0, Max(plan->nsegs, 1) * sizeof(RankSeg));
	r->unit = NULL;
	r->nunit = 0;
	r->units_built = false;
	sift_plan_prefetch_blobs(plan);
	for (int s = 0; s < plan->nsegs; s++)
	{
		rank_open_seg(r, s);
		sift_sr_release(&plan->seg[s]->sr);
	}
	r->mode = rank_choose_mode_fast(r);
	if (r->mode == SIFT_RM_BATCH_BMW)
		rank_materialize_units(r);
}

void
rank_widen(SiftRanker *r, float4 theta)
{
	sift_plan_widen(r->plan, theta);
	r->deferred = !sift_plan_tail_settled(r->plan, -INFINITY);
	rank_open(r);
}

SiftRanker *
sift_rank_begin(SiftPlan *plan, int64 k, SiftVerifyFn verify, void *arg)
{
	SiftRanker *r = MemoryContextAllocZero(plan->mcxt, sizeof(SiftRanker));

	Assert(plan->scoring);
	r->plan = plan;
	r->floor = -INFINITY;
	r->verify = verify;
	r->arg = arg;
	r->k = k;
	r->tmpcx = sift_mem_allocset_create(plan->mcxt, "sift candidate");
	r->wk = MemoryContextAllocAligned(plan->mcxt, sizeof(RankWork), SIFT_SIMD_ALIGN, MCXT_ALLOC_ZERO);
	r->leafub = MemoryContextAllocZero(plan->mcxt, Max(plan->nleaves, 1) * sizeof(float4));
	r->ord = MemoryContextAllocZero(plan->mcxt, Max(plan->nleaves, 1) * sizeof(int));
	r->rest = MemoryContextAllocZero(plan->mcxt, Max(plan->nleaves, 1) * sizeof(float4));
	r->seg = MemoryContextAllocZero(plan->mcxt, Max(plan->nsegs, 1) * sizeof(RankSeg));
	if (plan->nphr > 0)
		r->phrpos = posctx_create(plan);
	if (!sift_plan_warm_begin(plan, k) && (k <= 0 || sift_plan_tail_doomed(plan)))
		sift_plan_widen(plan, -INFINITY);
	r->deferred = !sift_plan_tail_settled(plan, -INFINITY);
	rank_open(r);
	return r;
}

void
sift_rank_parallel(SiftRanker *r, SiftPar *par)
{
	uint64		sig = 0;

	Assert(r->k > 0);
	if (r->deferred)
		rank_widen(r, -INFINITY);
	if (!r->units_built)
		rank_materialize_units(r);
	rank_sort_units(r);
	for (int i = 0; i < r->nunit; i++)
	{
		uint32		ub;

		memcpy(&ub, &r->unit[i].maxub, sizeof(ub));
		sig = hash_combine64(sig, (uint64) r->unit[i].seg << 32 | r->unit[i].gid);
		sig = hash_combine64(sig, ub);
	}
	r->par = par;
	sift_par_attach(par);
	sift_par_check_units(par, r->nunit, sig);
}
