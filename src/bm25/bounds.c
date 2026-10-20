/*
 * bm25/bounds.c - opens the ranker: the score bounds of the leaves per superblock and the units in descending bound order.
 */
#include "core/host.h"

#include <math.h>

#include "bm25/eval.h"
#include "bm25/eval_int.h"
#include "bm25/kernels.h"
#include "bm25/wbuf.h"
#include "bm25/prefix.h"
#include "bm25/segment.h"

static int
gidkey_cmp(const void *a, const void *b)
{
	return pg_cmp_u64(*(const uint64 *) a, *(const uint64 *) b);
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
	uint64	   *key;
	double		ref = p->seg[rs->s]->hdr->avgdl_ref;

	for (int i = 0; i < nt; i++)
	{
		int			n;

		sift_post_bounds(rs->cs.leafpc[leaf][i], &n);
		total += n;
	}
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
	/* sorts (gid, index) keys instead of the 68-byte entries */
	key = MemoryContextAlloc(cx, Max(total, 1) * sizeof(uint64));
	for (int i = 0; i < total; i++)
		key[i] = (uint64) all[i].gid << 32 | i;
	if (nt > 1)
		qsort(key, total, sizeof(uint64), gidkey_cmp);
	out->list = MemoryContextAlloc(cx, Max(total, 1) * sizeof(GidUB));
	for (int i = 0; i < total; i++)
	{
		const GidUB *in = &all[(uint32) key[i]];

		if (out->n > 0 && out->list[out->n - 1].gid == in->gid)
			for (int k = 0; k < SIFT_BLKS_PER_SB; k++)
				out->list[out->n - 1].ub[k] = Max(out->list[out->n - 1].ub[k], in->ub[k]);
		else
			out->list[out->n++] = *in;
	}
	pfree(key);
	pfree(all);
}

static bool
leaf_at(const LeafUB *l, uint32 gid)
{
	return l->pos < l->n && l->list[l->pos].gid == gid;
}

/* Does the bounded tree have a possible match in the SB the leaves' lists point at? Leaves that do not score have no list: they may match anywhere */
static bool
tree_present(const SiftPlan *p, const EvalNode *n, const LeafUB *lub, uint32 gid)
{
	switch (n->kind)
	{
		case EN_LEAF:
			return !p->leaves[n->leaf].scoring || leaf_at(&lub[n->leaf], gid);
		case EN_AND:
			for (int i = 0; i < n->nchild; i++)
				if (n->child[i]->kind != EN_NOT && !tree_present(p, n->child[i], lub, gid))
					return false;
			return true;
		case EN_OR:
			for (int i = 0; i < n->nchild; i++)
				if (tree_present(p, n->child[i], lub, gid))
					return true;
			return false;
		default:
			return false;
	}
}

/* Units of one segment, ascending gid: the leaf lists merged and the tree evaluated per SB */
static void
seg_units(SiftRanker *r, RankSeg *rs, Unit **units, int *nunits, int *cap)
{
	SiftPlan   *p = r->plan;

	for (;;)
	{
		uint32		gid = UINT32_MAX;
		Unit		u = {.seg = rs->s};
		const SiftFilterSB *fs;

		for (int i = 0; i < p->nleaves; i++)
			if (rs->lub[i].pos < rs->lub[i].n)
				gid = Min(gid, rs->lub[i].list[rs->lub[i].pos].gid);
		if (gid == UINT32_MAX)
			return;
		u.gid = gid;
		fs = p->filter ? sift_filter_sb(p->filter, gid) : NULL;
		if ((fs != NULL || p->filter == NULL) && tree_present(p, p->root, rs->lub, gid))
		{
			for (int i = 0; i < p->nleaves; i++)
				if (leaf_at(&rs->lub[i], gid))
					for (int k = 0; k < SIFT_BLKS_PER_SB; k++)
						u.ub[k] += rs->lub[i].list[rs->lub[i].pos].ub[k];
			for (int k = 0; k < SIFT_BLKS_PER_SB; k++)
			{
				/* a block without a page the filter keeps scores nothing */
				if (fs && !sift_filter_has_block(fs, k))
					u.ub[k] = 0;
				u.maxub = Max(u.maxub, u.ub[k]);
				u.nblk += u.ub[k] > 0;
			}
			if (*nunits == *cap)
			{
				*cap = Max(64, 2 * *cap);
				*units = *units ? repalloc_huge(*units, *cap * sizeof(Unit)) :
					MemoryContextAllocHuge(p->mcxt, *cap * sizeof(Unit));
			}
			(*units)[(*nunits)++] = u;
		}
		for (int i = 0; i < p->nleaves; i++)
			if (leaf_at(&rs->lub[i], gid))
				rs->lub[i].pos++;
	}
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

/* Per-term bound weights of a scoring leaf, and the largest ub of the leaf */
static void
term_bounds(SiftPlan *p, RankSeg *rs, int leaf)
{
	double		ref = p->seg[rs->s]->hdr->avgdl_ref;

	for (int j = 0; j < p->leaves[leaf].nterms[rs->s]; j++)
	{
		RkTerm	   *t = &rs->term[rs->first[leaf] + j];
		uint8		top = 0;

		t->gb = sift_post_bounds(t->pc, &t->ngb);
		for (int k = 0; k < t->ngb; k++)
			top = Max(top, t->gb[k].bound);
		t->ubw = sift_kern_ub_weight(t->w, p->stats.avgdl, ref, p->nleaves);
		t->tub = t->ubw * (float4) top;
		rs->lmax[leaf] = Max(rs->lmax[leaf], t->tub);
	}
}

static void
rank_open_seg(SiftRanker *r, int s, MemoryContext cx)
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
	rs->lub = MemoryContextAllocZero(cx, Max(p->nleaves, 1) * sizeof(LeafUB));
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
			leaf_bounds(p, rs, i, cx);
		}
}

SiftRanker *
sift_rank_begin(SiftPlan *plan, int64 k, SiftVerifyFn verify, void *arg)
{
	SiftRanker *r = MemoryContextAllocZero(plan->mcxt, sizeof(SiftRanker));
	MemoryContext cx = AllocSetContextCreate(CurrentMemoryContext, "sift bounds", ALLOCSET_DEFAULT_SIZES);
	int			cap = 0;

	Assert(plan->scoring);
	r->plan = plan;
	r->verify = verify;
	r->arg = arg;
	r->k = k;
	r->tmpcx = AllocSetContextCreate(plan->mcxt, "sift candidate", ALLOCSET_DEFAULT_SIZES);
	r->wk = MemoryContextAllocAligned(plan->mcxt, sizeof(RankWork), SIFT_SIMD_ALIGN, MCXT_ALLOC_ZERO);
	r->leafub = MemoryContextAllocZero(plan->mcxt, Max(plan->nleaves, 1) * sizeof(float4));
	r->ord = MemoryContextAllocZero(plan->mcxt, Max(plan->nleaves, 1) * sizeof(int));
	r->rest = MemoryContextAllocZero(plan->mcxt, Max(plan->nleaves, 1) * sizeof(float4));
	r->seg = MemoryContextAllocZero(plan->mcxt, Max(plan->nsegs, 1) * sizeof(RankSeg));
	if (plan->nphr > 0)
		r->phrpos = posctx_create(plan);
	for (int s = 0; s < plan->nsegs; s++)
	{
		rank_open_seg(r, s, cx);
		if (r->seg[s].root)
			seg_units(r, &r->seg[s], &r->unit, &r->nunit, &cap);
		sift_sr_release(&plan->seg[s]->sr);
	}
	qsort(r->unit, r->nunit, sizeof(Unit), unit_cmp);
	MemoryContextDelete(cx);
	return r;
}

void
sift_rank_parallel(SiftRanker *r, SiftPar *par)
{
	uint64		sig = 0;

	Assert(r->k > 0);
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
