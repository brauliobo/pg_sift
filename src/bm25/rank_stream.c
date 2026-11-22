/*
 * bm25/rank_stream.c - the units of a MaxScore or conjunctive search, made one at a time in ascending (segment, gid) order.
 *
 * No list of units is kept and no list of a leaf's superblocks is merged: a cursor per scoring leaf walks the bounds its terms already have
 * (RkTerm.gb), and a unit is made from the cursors that stand on its superblock. The leaves with the smallest bounds are the non-essential
 * ones while their sum cannot reach theta: no superblock is a unit for them alone, and they are only looked at in a superblock an essential
 * leaf has, after the essential bounds alone left it worth a look.
 */
#include "rank_int.h"

/* A walk in ascending gid over the superblocks one scoring leaf has postings in, in one segment */
typedef struct SiftLeafBoundCur
{
	int			leaf;
	float4		lmax;			/* the largest ub of the leaf in the segment */
	int			nterms;
	const RkTerm *terms;
	int		   *tpos;			/* [nterms] the next entry of each term's bounds */
	uint32		cur_gid;		/* the smallest gid at the terms' positions, UINT32_MAX when they are all done */
} SiftLeafBoundCur;

struct UnitStream
{
	SiftRanker *r;
	RankSeg    *rs;
	const Unit *seed;			/* the units ranked already, ascending by (segment, gid): the walk leaves them out */
	int			nseed;
	int			seed_i;			/* the first seed the walk has not passed */
	int			ncur;			/* cursors of the scoring leaves that have terms in the segment, by ascending lmax */
	SiftLeafBoundCur *cur;
	int			nreq;			/* a conjunctive search is driven by the leaves its root AND requires: their cursors */
	SiftLeafBoundCur **req;
	bool		dead;			/* a required leaf has no term in the segment: no unit */
	float4		seg_ub;			/* the sum of the leaves' lmax */
	uint32		bridge_i;		/* an inverted segment: the next superblock of the bridge */
	const float4 **ubp;			/* [leaf] scratch: the bounds of the unit being made, NULL for no posting */
	float4	  (*ubuf)[SIFT_BLKS_PER_SB];
};

/* The first entry at or after pos whose gid is not below target: a gallop, then a bisection of the last step */
static int
gb_lower(const SiftGroupBounds *gb, int n, int pos, uint32 target)
{
	int			lo = pos,
				hi,
				step = 1;

	if (pos >= n || gb[pos].gid >= target)
		return pos;
	while (lo + step < n && gb[lo + step].gid < target)
	{
		lo += step;
		step *= 2;
	}
	hi = Min(lo + step, n);
	while (lo + 1 < hi)
	{
		int			mid = lo + (hi - lo) / 2;

		if (gb[mid].gid < target)
			lo = mid;
		else
			hi = mid;
	}
	return hi;
}

static void
lbcur_settle(SiftLeafBoundCur *c)
{
	c->cur_gid = UINT32_MAX;
	for (int i = 0; i < c->nterms; i++)
		if (c->tpos[i] < c->terms[i].ngb)
			c->cur_gid = Min(c->cur_gid, c->terms[i].gb[c->tpos[i]].gid);
}

static void
lbcur_advance_to(SiftLeafBoundCur *c, uint32 target)
{
	if (c->cur_gid >= target)
		return;
	for (int i = 0; i < c->nterms; i++)
		c->tpos[i] = gb_lower(c->terms[i].gb, c->terms[i].ngb, c->tpos[i], target);
	lbcur_settle(c);
}

/* The leaf's bounds in superblock gid, which the cursor stands on: the max over its terms there, as leaf_bounds merges them */
static void
lbcur_ub(const SiftLeafBoundCur *c, uint32 gid, float4 *ub)
{
	memset(ub, 0, SIFT_BLKS_PER_SB * sizeof(float4));
	if (c->nterms == 1)
	{
		const RkTerm *t = &c->terms[0];

		if (c->tpos[0] < t->ngb && t->gb[c->tpos[0]].gid == gid)
			sift_kern->ub_accum(ub, t->gb[c->tpos[0]].blk_bound, t->ubw, SIFT_BLKS_PER_SB);
		return;
	}
	for (int i = 0; i < c->nterms; i++)
	{
		const RkTerm *t = &c->terms[i];
		float4		v[SIFT_BLKS_PER_SB] = {0};

		if (c->tpos[i] >= t->ngb || t->gb[c->tpos[i]].gid != gid)
			continue;
		sift_kern->ub_accum(v, t->gb[c->tpos[i]].blk_bound, t->ubw, SIFT_BLKS_PER_SB);
		for (int k = 0; k < SIFT_BLKS_PER_SB; k++)
			ub[k] = Max(ub[k], v[k]);
	}
}

static float4
ub_max(const float4 *ub)
{
	float4		m = 0;

	for (int k = 0; k < SIFT_BLKS_PER_SB; k++)
		m = Max(m, ub[k]);
	return m;
}

static bool
leaf_required(const SiftPlan *p, int leaf)
{
	if (p->root->kind != EN_AND)
		return false;
	for (int i = 0; i < p->root->nchild; i++)
		if (p->root->child[i]->kind == EN_LEAF && p->root->child[i]->leaf == leaf)
			return true;
	return false;
}

static UnitStream *
stream_make(SiftRanker *r, int s)
{
	SiftPlan   *p = r->plan;
	RankSeg    *rs = &r->seg[s];
	MemoryContext cx = p->mcxt;
	UnitStream *st = MemoryContextAllocZero(cx, sizeof(UnitStream));
	int			nleaves = Max(p->nleaves, 1);

	st->r = r;
	st->rs = rs;
	st->cur = MemoryContextAllocZero(cx, nleaves * sizeof(SiftLeafBoundCur));
	st->req = MemoryContextAllocZero(cx, nleaves * sizeof(SiftLeafBoundCur *));
	st->ubp = MemoryContextAllocZero(cx, nleaves * sizeof(float4 *));
	st->ubuf = MemoryContextAllocZero(cx, nleaves * sizeof(*st->ubuf));
	for (int i = 0; i < p->nleaves; i++)
	{
		SiftLeafBoundCur *c;
		int			j;

		if (!p->leaves[i].scoring)
			continue;
		st->seg_ub += rs->lmax[i];
		if (p->leaves[i].nterms[s] == 0)
		{
			st->dead |= r->mode == SIFT_RM_CONJUNCTIVE && leaf_required(p, i);
			continue;
		}
		for (j = st->ncur; j > 0 && st->cur[j - 1].lmax > rs->lmax[i]; j--)
			st->cur[j] = st->cur[j - 1];
		c = &st->cur[j];
		*c = (SiftLeafBoundCur) {.leaf = i,.lmax = rs->lmax[i],.nterms = p->leaves[i].nterms[s],.terms = rs->term + rs->first[i]};
		c->tpos = MemoryContextAllocZero(cx, c->nterms * sizeof(int));
		st->ncur++;
	}
	if (r->mode == SIFT_RM_CONJUNCTIVE)
		for (int k = 0; k < st->ncur; k++)
			if (leaf_required(p, st->cur[k].leaf))
				st->req[st->nreq++] = &st->cur[k];
	rs->stream = st;
	return st;
}

static UnitStream *
seg_stream(SiftRanker *r, int s)
{
	return r->seg[s].stream ? r->seg[s].stream : stream_make(r, s);
}

/* The unit of superblock gid of the segment from the terms' own lists, for a unit that is not met in the walk (a seed) */
static bool
unit_at(UnitStream *st, uint32 gid, Unit *u)
{
	const SiftPlan *p = st->r->plan;

	for (int i = 0; i < p->nleaves; i++)
		st->ubp[i] = p->leaves[i].scoring && leaf_bound_at(p, st->rs, i, gid, st->ubuf[i]) ? st->ubuf[i] : NULL;
	return unit_make(st->r, st->rs->s, gid, st->ubp, u);
}

/* The leaf with the largest bound in some segment that is not in top[0 .. n - 1], -1 for none */
static int
seed_leaf(const SiftRanker *r, const int *top, int n)
{
	const SiftPlan *p = r->plan;
	int			best = -1;
	float4		bestv = -1;

	for (int i = 0; i < p->nleaves; i++)
	{
		float4		v = -1;
		bool		taken = false;

		for (int k = 0; k < n; k++)
			taken |= top[k] == i;
		if (!p->leaves[i].scoring || taken)
			continue;
		for (int s = 0; s < p->nsegs; s++)
			if (r->seg[s].root)
				v = Max(v, r->seg[s].lmax[i]);
		if (v > bestv)
		{
			best = i;
			bestv = v;
		}
	}
	return best;
}

typedef struct SeedEnt
{
	float4		ub;
	uint32		seg;
	uint32		gid;
} SeedEnt;

static int
seed_cmp(const void *a, const void *b)
{
	const SeedEnt *x = a,
			   *y = b;

	if (x->seg != y->seg)
		return x->seg < y->seg ? -1 : 1;
	return x->gid < y->gid ? -1 : x->gid > y->gid;
}

static int
seed_worst(const SeedEnt *ent)
{
	int			w = 0;

	for (int i = 1; i < RANK_SEED; i++)
		if (ent[i].ub < ent[w].ub)
			w = i;
	return w;
}

/*
 * The (up to RANK_SEED) units that the leaves with the largest bounds, three of them, have their best superblocks in, in ascending order; the
 * threshold is near its final value before the sweep, which then leaves them out. Their bound is that of the one leaf, which is all
 * that is read of the other leaves' lists.
 */
int
rank_seed_units(SiftRanker *r, Unit *seed)
{
	const SiftPlan *p = r->plan;
	SeedEnt		ent[RANK_SEED];
	int			top[3],
				ntop = 0,
				n = 0,
				worst = 0,
				nseed = 0;

	while (ntop < 3 && (top[ntop] = seed_leaf(r, top, ntop)) >= 0)
		ntop++;
	for (int s = 0; s < p->nsegs; s++)
		for (int k = 0; r->seg[s].root && k < ntop; k++)
			for (int j = 0; j < p->leaves[top[k]].nterms[s]; j++)
			{
				const RkTerm *t = &r->seg[s].term[r->seg[s].first[top[k]] + j];

				for (int g = 0; g < t->ngb; g++)
				{
					SeedEnt		e = {t->ubw * (float4) t->gb[g].bound, s, t->gb[g].gid};

					if (n < RANK_SEED)
						ent[n++] = e;
					else if (e.ub > ent[worst].ub)
						ent[worst] = e;
					else
						continue;
					if (n == RANK_SEED)
						worst = seed_worst(ent);
				}
			}
	qsort(ent, n, sizeof(SeedEnt), seed_cmp);
	for (int i = 0; i < n; i++)
		if ((i == 0 || seed_cmp(&ent[i - 1], &ent[i]) != 0) && unit_at(seg_stream(r, ent[i].seg), ent[i].gid, &seed[nseed]))
			nseed++;
	return nseed;
}

UnitStream *
unit_stream_open(SiftRanker *r, int s, const Unit *seed, int nseed)
{
	UnitStream *st;

	if (r->seg[s].root == NULL)
		return NULL;
	st = seg_stream(r, s);
	st->seed = seed;
	st->nseed = nseed;
	st->seed_i = 0;
	st->bridge_i = r->seg[s].inverted ? bridge_first_sb(r->plan->bridge, r->plan->seg[s]->hdr->sb_lo) : 0;
	for (int k = 0; k < st->ncur; k++)
	{
		memset(st->cur[k].tpos, 0, st->cur[k].nterms * sizeof(int));
		lbcur_settle(&st->cur[k]);
	}
	return st->dead ? NULL : st;
}

/* The walk takes the superblocks in ascending order, as the seeds are listed: the seeds before it are passed for good */
static bool
stream_seeded(UnitStream *st, uint32 gid)
{
	const Unit *seed = st->seed;
	int			s = st->rs->s;

	while (st->seed_i < st->nseed && (seed[st->seed_i].seg < s || (seed[st->seed_i].seg == s && seed[st->seed_i].gid < gid)))
		st->seed_i++;
	return st->seed_i < st->nseed && seed[st->seed_i].seg == s && seed[st->seed_i].gid == gid;
}

/* The next superblock of a bridge-driven segment, UINT32_MAX when there is none */
static uint32
stream_bridge_gid(UnitStream *st)
{
	const SiftTidBridge *br = st->r->plan->bridge;

	if (st->bridge_i >= br->nsbs || br->sbs[st->bridge_i].sb > st->r->plan->seg[st->rs->s]->hdr->sb_hi)
		return UINT32_MAX;
	return br->sbs[st->bridge_i].sb;
}

/* The next superblock all the leaves a root AND requires have postings in, UINT32_MAX when there is none */
static uint32
stream_aligned_gid(UnitStream *st)
{
	for (;;)
	{
		uint32		gid = 0;
		bool		same = true;

		for (int k = 0; k < st->nreq; k++)
			gid = Max(gid, st->req[k]->cur_gid);
		if (gid == UINT32_MAX)
			return gid;
		for (int k = 0; k < st->nreq; k++)
		{
			lbcur_advance_to(st->req[k], gid);
			same &= st->req[k]->cur_gid == gid;
		}
		if (same)
			return gid;
	}
}

/*
 * The first leaf (cursors are by ascending lmax) whose bound, with those below it, can lift a document over theta, and the smallest gid
 * those from it up have postings in: the superblocks only the leaves below it have are no unit. The last leaf is always essential.
 */
static uint32
stream_union_gid(const UnitStream *st, float4 theta, int *pivot, float4 *below)
{
	uint32		gid = UINT32_MAX;

	*pivot = 0;
	*below = 0;
	while (*pivot < st->ncur - 1 && *below + st->cur[*pivot].lmax <= theta)
		*below += st->cur[(*pivot)++].lmax;
	for (int k = *pivot; k < st->ncur; k++)
		gid = Min(gid, st->cur[k].cur_gid);
	return gid;
}

/* The unit of superblock gid, false when no unit is worth ranking; the leaves below the pivot are looked at only if the others do not rule it out */
static bool
stream_unit(UnitStream *st, uint32 gid, int pivot, float4 below, float4 theta, Unit *u)
{
	SiftPlan   *p = st->r->plan;
	float4		ub = below;

	memset(st->ubp, 0, Max(p->nleaves, 1) * sizeof(float4 *));
	for (int k = pivot; k < st->ncur; k++)
	{
		SiftLeafBoundCur *c = &st->cur[k];

		lbcur_advance_to(c, gid);
		if (c->cur_gid != gid)
			continue;
		lbcur_ub(c, gid, st->ubuf[c->leaf]);
		st->ubp[c->leaf] = st->ubuf[c->leaf];
		ub += ub_max(st->ubuf[c->leaf]);
	}
	if (pivot > 0 && !(ub > theta))
		return false;
	for (int k = 0; k < pivot; k++)
	{
		SiftLeafBoundCur *c = &st->cur[k];

		lbcur_advance_to(c, gid);
		if (c->cur_gid != gid)
			continue;
		lbcur_ub(c, gid, st->ubuf[c->leaf]);
		st->ubp[c->leaf] = st->ubuf[c->leaf];
	}
	return unit_make(st->r, st->rs->s, gid, st->ubp, u);
}

/* The next unit of the segment whose bound is above the threshold, in ascending gid; false at the end of the segment */
bool
unit_stream_next(UnitStream *st, const SiftTopK *tk, Unit *u)
{
	SiftRanker *r = st->r;
	RankSeg    *rs = st->rs;

	for (;;)
	{
		float4		theta = sift_kern_topk_theta(tk),
					below = 0;
		int			pivot = 0;
		uint32		gid;
		bool		made = false;

		CHECK_FOR_INTERRUPTS();
		if (!(st->seg_ub > theta))
			return false;
		if (rs->inverted)
			gid = stream_bridge_gid(st);
		else if (st->nreq > 0)
			gid = stream_aligned_gid(st);
		else
			gid = stream_union_gid(st, theta, &pivot, &below);
		if (gid == UINT32_MAX)
			return false;
		if (!stream_seeded(st, gid))
			made = rs->inverted ? unit_at(st, gid, u) : stream_unit(st, gid, pivot, below, theta, u);
		if (rs->inverted)
			st->bridge_i++;
		else
			for (int k = 0; k < st->ncur; k++)
				if (st->cur[k].cur_gid == gid)
					lbcur_advance_to(&st->cur[k], gid + 1);
		if (made && !(u->maxub > theta))
		{
			r->plan->stats.blocks_skipped += u->nblk;
			made = false;
		}
		if (made)
			return true;
	}
}
