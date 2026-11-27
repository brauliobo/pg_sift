/*
 * bm25/phrase.c - phrases decided by stored positions, their cursors, and the recheck of candidates against their text.
 */
#include "core/host.h"

#include "bm25/eval/eval.h"
#include "bm25/eval/int.h"
#include "bm25/kernels/kernels.h"

PosCtx *
posctx_create(SiftPlan *p)
{
	PosCtx	   *c = MemoryContextAllocZero(p->mcxt, sizeof(PosCtx));
	int			n = Max(p->nleaves, 1);

	c->plan = p;
	c->mcxt = p->mcxt;
	c->pc = MemoryContextAllocZero(p->mcxt, n * sizeof(SiftPostingCursor *));
	c->g = MemoryContextAllocZero(p->mcxt, n * sizeof(sift_grp *));
	c->wpage = MemoryContextAllocZero(p->mcxt, n * sizeof(int));
	c->widx = MemoryContextAllocZero(p->mcxt, n * sizeof(uint32));
	c->wrank = MemoryContextAllocZero(p->mcxt, n * sizeof(uint32));
	c->pb = MemoryContextAllocZero(p->mcxt, n * sizeof(SiftPosBuf));
	c->stamp = MemoryContextAllocZero(p->mcxt, n * sizeof(uint64));
	for (int i = 0; i < n; i++)
		c->pb[i].mcxt = p->mcxt;
	return c;
}

static inline uint32
pages_below(const uint64 *pages, int page)
{
	uint32		n = 0;

	for (int wd = 0; wd < page / 64; wd++)
		n += pg_popcount64(pages[wd]);
	return n + pg_popcount64(pages[page / 64] & ((UINT64CONST(1) << (page & 63)) - 1));
}

/* Rank in the term of the current doc, which the term's group holds; the walk keeps the pages behind it */
static uint32
posctx_rank(PosCtx *c, int leaf)
{
	const sift_grp *g = c->g[leaf];
	uint32		idx;
	const uint8 *m;

	Assert(g->pages[c->page / 64] >> (c->page & 63) & 1);
	if (c->page < c->wpage[leaf])
		c->wpage[leaf] = c->widx[leaf] = c->wrank[leaf] = 0;
	idx = pages_below(g->pages, c->page);
	c->wrank[leaf] += sift_popcount(g->off + (size_t) c->widx[leaf] * g->w, (size_t) (idx - c->widx[leaf]) * g->w);
	c->widx[leaf] = idx;
	c->wpage[leaf] = c->page;
	m = g->off + (size_t) idx * g->w;
	return g->base + c->wrank[leaf] + sift_popcount(m, c->off >> 3) + pg_popcount32(m[c->off >> 3] & ((1 << (c->off & 7)) - 1));
}

static const SiftPosBuf *
posctx_positions(PosCtx *c, int leaf)
{
	if (c->stamp[leaf] != c->doc)
	{
		sift_post_positions(c->pc[leaf], posctx_rank(c, leaf), &c->pb[leaf]);
		c->stamp[leaf] = c->doc;
	}
	return &c->pb[leaf];
}

/* The first index at or after j whose position is >= want (n when none): doubling steps, then a bisection of the last one */
static inline uint32
pos_gallop(const uint32 *v, uint32 n, uint32 j, uint32 want)
{
	uint32		step = 1,
				lo;

	if (j >= n || v[j] >= want)
		return j;
	lo = j;
	while (lo + step < n && v[lo + step] < want)
	{
		lo += step;
		step <<= 1;
	}
	for (uint32 hi = Min(lo + step, n); lo + 1 < hi;)
	{
		uint32		mid = lo + (hi - lo) / 2;

		if (v[mid] < want)
			lo = mid;
		else
			hi = mid;
	}
	return lo + 1;
}

/* A rarest term with more positions than this in a doc may not be the one with the fewest there */
#define PHRASE_DRIVE_MIN	4

/* Two terms, the common phrase: the first start both hold decides, no list of starts is built */
static bool
phrase_match2(PosCtx *c, const PhraseCheck *ph)
{
	uint32		oa = ph->offs ? ph->offs[ph->order[0]] : ph->order[0],
				ob = ph->offs ? ph->offs[ph->order[1]] : ph->order[1],
				j = 0;
	const SiftPosBuf *pa = posctx_positions(c, ph->leaf[ph->order[0]]),
			   *pb = posctx_positions(c, ph->leaf[ph->order[1]]);

	/* the term with fewer positions in this doc drives: the other is galloped into */
	if (pa->n > pb->n)
	{
		const SiftPosBuf *tp = pa;
		uint32		to = oa;

		pa = pb, pb = tp;
		oa = ob, ob = to;
	}
	for (uint32 x = 0; x < pa->n; x++)
	{
		uint32		want;

		if (pa->v[x] < oa)
			continue;
		want = pa->v[x] - oa + ob;
		j = pos_gallop(pb->v, pb->n, j, want);
		if (j == pb->n)
			return false;
		if (pb->v[j] == want)
			return true;
	}
	return false;
}

/* Does the current doc hold the phrase: the starts that fit the terms read so far, narrowed term by term, rarest first */
static bool
phrase_match(PosCtx *c, const PhraseCheck *ph)
{
	uint32		n = 0;
	int			drive = 0;

	CHECK_FOR_INTERRUPTS();
	if (ph->nterms == 2)
		return phrase_match2(c, ph);
	/* the starts come from the term with the fewest positions in this doc (the rarest by df, unless the doc has it often), the others narrow them in order */
	if (posctx_positions(c, ph->leaf[ph->order[0]])->n > PHRASE_DRIVE_MIN)
		for (int k = 1; k < ph->nterms; k++)
			if (posctx_positions(c, ph->leaf[ph->order[k]])->n < posctx_positions(c, ph->leaf[ph->order[drive]])->n)
				drive = k;
	for (int s = 0; s < ph->nterms; s++)
	{
		int			k = s == 0 ? drive : s - 1 < drive ? s - 1 : s;
		uint32		i = ph->order[k],
					o = ph->offs ? ph->offs[i] : i,
					j = 0,
					m = 0;
		const SiftPosBuf *pb = posctx_positions(c, ph->leaf[i]);

		if (s == 0)
		{
			if (pb->n > c->startcap)
			{
				c->startcap = Max(pb->n, 2 * c->startcap);
				SIFT_REGROW_HUGE_IN(c->mcxt, c->start, (Size) c->startcap * sizeof(uint32));
			}
			for (; j < pb->n; j++)
				if (pb->v[j] >= o)
					c->start[m++] = pb->v[j] - o;
		}
		else
			for (uint32 x = 0; x < n; x++)
			{
				uint32		want = c->start[x] + o;

				j = pos_gallop(pb->v, pb->n, j, want);
				if (j == pb->n)
					break;
				if (pb->v[j] == want)
					c->start[m++] = c->start[x];
			}
		if ((n = m) == 0)
			return false;
	}
	return true;
}

/* Is no required phrase the positions of segment s decide missing from the doc at (page of the SB, offset - 1) */
bool
phrases_hold(PosCtx *c, int s, int page, int off)
{
	const SiftPlan *p = c->plan;

	c->doc++;
	c->page = page;
	c->off = off;
	for (int i = 0; i < p->nphr; i++)
		if (p->phr[i].required && p->phok[s * p->nphr + i] && !phrase_match(c, &p->phr[i]))
			return false;
	return true;
}

/* An optional phrase: the docs of its relaxation (the AND of its terms) that hold the phrase */
typedef struct PhraseCur
{
	sift_cur	c;				/* first: cursors are used through it */
	sift_cur   *kidp[1];
	const IterCurs *ic;
	int			ph;
} PhraseCur;

/* Copies the docs of g that hold the phrase into the cursor's group; false when there are none */
static bool
phrase_group(PhraseCur *pc, const sift_grp *g)
{
	PosCtx	   *ctx = pc->ic->pos;
	const PhraseCheck *ph = &ctx->plan->phr[pc->ph];
	const uint8 *src = g->off;
	uint8	   *dst = pc->c.buf;
	uint64		pages[SIFT_PAGEMASK_WORDS] = {0};

	if ((g->pages[0] | g->pages[1] | g->pages[2] | g->pages[3]) == 0)
		return false;
	for (int i = 0; i < ph->nterms; i++)
	{
		SiftPostingCursor *tc = pc->ic->leafpc[ph->leaf[i]][0];
		const sift_cur *cur = sift_post_cursor(tc);

		SIFT_INVARIANT(cur->state == 1 && cur->g.gid == g->gid, "sift: a phrase term is not positioned at superblock %u", g->gid);
		posctx_group(ctx, ph->leaf[i], tc, &cur->g);
	}
	for (int wd = 0; wd < SIFT_PAGEMASK_WORDS; wd++)
		for (uint64 m = g->pages[wd]; m; m &= m - 1, src += g->w)
		{
			int			page = wd * 64 + pg_rightmost_one_pos64(m);
			uint64		mask[SIFT_OFFMASK_WORDS] = {0};
			uint64		any = 0;

			memcpy(mask, src, g->w);
			for (int k = 0; k < SIFT_OFFMASK_WORDS; k++)
				for (uint64 b = mask[k]; b; b &= b - 1)
				{
					ctx->doc++;
					ctx->page = page;
					ctx->off = k * 64 + pg_rightmost_one_pos64(b);
					if (!phrase_match(ctx, ph))
						mask[k] &= ~(b & -b);
				}
			for (int k = 0; k < SIFT_OFFMASK_WORDS; k++)
				any |= mask[k];
			if (!any)
				continue;
			pages[wd] |= UINT64CONST(1) << (page & 63);
			memcpy(dst, mask, g->w);
			dst += g->w;
		}
	if ((pages[0] | pages[1] | pages[2] | pages[3]) == 0)
		return false;
	pc->c.g = (sift_grp) {.gid = g->gid,.w = g->w,.base = 0,.n = 0,.off = pc->c.buf};
	memcpy(pc->c.g.pages, pages, sizeof(pages));
	return true;
}

static const sift_grp *
phrase_seek(sift_cur *c, uint32_t target)
{
	PhraseCur  *pc = (PhraseCur *) c;

	if (c->state == 2)
		return NULL;
	if (c->state == 1 && c->g.gid >= target)
		return &c->g;
	for (const sift_grp *g; (g = sift_seek(pc->kidp[0], target)); target = g->gid + 1)
	{
		CHECK_FOR_INTERRUPTS();
		if (phrase_group(pc, g))
		{
			c->state = 1;
			return &c->g;
		}
	}
	c->state = 2;
	return NULL;
}

/* kid filtered by phrase ph when the segment's positions decide it; kid itself otherwise */
sift_cur *
phrase_cursor(SiftPlan *p, int s, int ph, sift_cur *kid, const IterCurs *ic, MemoryContext cx)
{
	PhraseCur  *pc;

	if (kid == NULL || ic == NULL || !p->phok[s * p->nphr + ph])
		return kid;
	pc = MemoryContextAllocZero(cx, sizeof(PhraseCur));
	pc->c.seek = phrase_seek;
	pc->c.buf = cursor_buf(cx);
	pc->c.ub1 = 255;
	pc->kidp[0] = kid;
	pc->c.op.kid = pc->kidp;
	pc->c.op.k = 1;
	pc->ic = ic;
	pc->ph = ph;
	return &pc->c;
}

/* Best term of the leaf in a document's text, weighed like the others; a wide leaf has one term, with the tf of all its words */
static float4
doc_leaf_score(const EvalLeaf *l, SiftDoc *d, float4 kn)
{
	float4		best = 0;

	if (l->wide)
	{
		int			tf = sift_doc_leaf_tf(d, l->node);

		return tf > 0 ? sift_kern_bm25(leaf_weighed(l, l->wide, l->widelen)->w, Min(tf, SIFT_MAX_TF), kn) : 0;
	}
	for (int j = 0; j < l->nuniq; j++)
	{
		int			tf = sift_doc_term_tf(d, l->uniq[j]->term, l->uniq[j]->len);

		if (tf > 0)
			best = Max(best, sift_kern_bm25(l->uniq[j]->w, Min(tf, SIFT_MAX_TF), kn));
	}
	return best;
}

/* The exact score of a document from its text, for plans whose phrases are optional */
static float4
doc_score(SiftPlan *p, SiftDoc *d)
{
	float4		kn = sift_kern_knorm1(&p->bm, Min(sift_doc_ntokens(d), SIFT_MAX_DL)),
				score = 0;

	for (int i = 0; i < p->nleaves; i++)
	{
		const EvalLeaf *l = &p->leaves[i];

		if (l->scoring && !(l->phrase && !sift_doc_match(d, l->phrase)))
			score += doc_leaf_score(l, d, l->field && p->fscored ?
									sift_kern_knorm1(&p->bmf[l->field - 1], sift_doc_field_dl(d, l->field)) : kn);
	}
	return score;
}

/* The exact recheck of a candidate's text; rescores in place when the index score is only a bound */
bool
doc_pass(SiftPlan *p, text *doc, float4 *score)
{
	SiftDoc    *d = sift_doc_open(p->st->analyzer, VARDATA_ANY(doc), VARSIZE_ANY_EXHDR(doc));

	for (int i = 0; i < p->nqueries; i++)
		if (!sift_doc_recheck(p->queries[i], d))
			return false;
	if (score && p->rescore)
		*score = doc_score(p, d);
	return true;
}

bool
sift_plan_eval_doc(SiftPlan *p, text *doc, float4 *score)
{
	SiftDoc    *d = sift_doc_open(p->st->analyzer, VARDATA_ANY(doc), VARSIZE_ANY_EXHDR(doc));

	for (int i = 0; i < p->nqueries; i++)
		if (!sift_doc_match(d, p->queries[i]->root))
			return false;
	*score = p->scoring ? doc_score(p, d) : 0;
	return true;
}

bool
sift_plan_recheck(SiftPlan *plan, text *doc)
{
	return doc_pass(plan, doc, NULL);
}

bool
sift_plan_recheck_needed(const SiftPlan *plan)
{
	return plan->heap;
}

/* The write buffer keeps no positions and a segment may lack them: a plan with a phrase needs the text of its hits at any execution, whatever it needs when it is bound */
bool
sift_plan_recheck_possible(const SiftPlan *plan)
{
	return plan->nphr > 0 || plan->recheck;
}
