/*
 * bm25/tail.c - the wide leaves: leaves with more terms than sift.max_expansions.
 *
 * A wide leaf cannot be an OR of cursors, one weight each. It reads as the one term that holds the documents with a word the leaf matches, in every segment
 * (bm25/prefix.c), unless its plan is ranked: then it is deferred, and its terms are the few with the largest bounds (bm25/wbind.c). The search of those finds from its
 * K-th score whether the other terms could change a result: a document that scores through a term left out has at most the term's bound and the largest scores
 * of the other leaves, so when that is not above the K-th score the document is not among the K best, and no document of them scores more through the term.
 *
 * When some terms could, the leaf is read again with the terms that can and no others, one segment at a time: as they are when they are few (WIDE_TOP_MAX), else as the
 * union of those only. The terms left out are never read; tail_mb of the leaf is the largest bound among them, which tells for which K-th scores the terms it reads
 * still decide. A leaf read without a K-th score (theta -infinity) has no term left out.
 */
#include "core/host.h"

#include <math.h>

#include "bm25/eval.h"
#include "bm25/eval_int.h"
#include "bm25/prefix.h"
#include "bm25/segment.h"
#include "bm25/wmemo.h"

/* The bounds carry the rounding of the weights: a score above theta needs a bound that clears it by more */
#define BOUND_SLACK		(1.0f + 1.0f / 65536)

#ifdef SIFT_TESTING
#define TAIL_STAT(f)	(sift_rank_stats.f++)
#else
#define TAIL_STAT(f)	((void) 0)
#endif

/*
 * The name of the term a wide prefix reads as (what indexed_leaf makes of a prefix of the leaf's field); the name of any other wide leaf is one
 * byte no word starts with, and its cache key (NULL when too long to be one) tells it from the others
 */
void
tail_wide_names(SiftPlan *p, EvalLeaf *l)
{
	const SiftQNode *n = l->node;
	char	   *key = MemoryContextAlloc(p->mcxt, n->textlen + 4);
	int			len = 0;

	if (n->kind == SQ_PREFIX)
	{
		if (n->field)
			key[len++] = SIFT_FIELD_BYTE(n->field);
		key[len++] = SIFT_PREFIX_SEP;
		memcpy(key + len, n->text, n->textlen);
		l->wide = l->wide_key = key;
		l->widelen = l->wide_keylen = len + n->textlen;
		return;
	}
	l->wide = pstrdup("\xFF");
	l->widelen = 1;
	key[len++] = 0xFF;
	key[len++] = n->kind;
	key[len++] = n->fuzzy;
	key[len++] = n->field;
	memcpy(key + len, n->text, n->textlen);
	l->wide_keylen = len + n->textlen;
	l->wide_key = l->wide_keylen > SIFT_MAX_TERM_BYTES + 2 ? NULL : key;
	if (l->wide_key == NULL)
		pfree(key);
}

/* Frees what the leaf reads in segment s: expanded terms, or the union term and its blob (its name is the leaf's) */
static void
leaf_release(EvalLeaf *l, int s)
{
	for (int i = 0; i < l->nterms[s]; i++)
	{
		LeafTerm   *t = &l->terms[s][i];

		if (t->blob)
			pfree((void *) t->blob);
		if (t->term != l->wide)
			pfree(t->term);
		if (t->info.flags & SIFT_TERM_INLINE && t->info.pos_len > 0)
			pfree((void *) t->info.inline_pos);
	}
	if (l->terms[s])
		pfree(l->terms[s]);
	l->nterms[s] = 0;
	l->terms[s] = NULL;
}

/* The leaf in segment s as the one term of the documents with a word it matches, the terms with a bound below min_bound left out (left_out: the largest bound of those) */
static void
union_seg(SiftPlan *p, EvalLeaf *l, int s, int min_bound, uint8 *left_out)
{
	SiftScoreRef ref = {(float4) p->st->dir->meta.k1, (float4) p->st->dir->meta.b, p->seg[s]->hdr->avgdl_ref};
	LeafTerm   *t = MemoryContextAllocZero(p->mcxt, sizeof(LeafTerm));

	t->blob = sift_prefix_union(p->seg[s], l->m, l->wide_key, l->wide_keylen, &ref, p->scoring && l->scoring, min_bound, p->mcxt, &t->info, left_out);
	t->term = l->wide;
	t->len = l->widelen;
	l->nterms[s] = t->info.df > 0;
	l->terms[s] = t;
}

void
tail_widen_leaf(SiftPlan *p, EvalLeaf *l)
{
	tail_wide_names(p, l);
	for (int s = 0; s < p->nsegs; s++)
	{
		leaf_release(l, s);
		union_seg(p, l, s, 0, NULL);
	}
}

/*
 * A wide leaf of a ranked plan whose score no phrase decides is deferred: its postings are not unioned at bind. The weighing keeps the few
 * terms with the largest bounds as the leaf's terms, and the search that reads those finds, from its K-th score, whether the other terms
 * could change a result (sift_plan_tail_settled): only when they could is the leaf read again (sift_plan_widen).
 */
void
tail_defer_leaf(SiftPlan *p, EvalLeaf *l)
{
	tail_wide_names(p, l);
	for (int s = 0; s < p->nsegs; s++)
		leaf_release(l, s);
	l->deferred = true;
	p->ndeferred++;
}

/* The largest score the leaf can give a document of segment s: its weight and the largest bound of any of its terms there */
static float4
leaf_ub_max(const SiftPlan *p, const EvalLeaf *l, int s)
{
	uint8		mb = 0;

	if (l->deferred)
		mb = l->all_mb[s];
	else
		for (int i = 0; i < l->nterms[s]; i++)
			mb = Max(mb, l->terms[s][i].info.max_bound);
	if (mb == 0)
		return 0;
	return sift_kern_ub_weight(l->deferred ? l->wide_w : l->terms[s][0].w, p->stats.avgdl, p->seg[s]->hdr->avgdl_ref, p->nleaves) * (float4) mb;
}

/* The largest score a term of bound mb of leaf i gives a document of segment s, and the other leaves give it at most */
static float4
term_bound(const SiftPlan *p, int i, int s, int mb)
{
	const EvalLeaf *l = &p->leaves[i];
	float4		others = 0;

	for (int j = 0; j < p->nleaves; j++)
		if (j != i && p->leaves[j].scoring)
			others += leaf_ub_max(p, &p->leaves[j], s);
	return sift_kern_ub_weight(l->wide_w, p->stats.avgdl, p->seg[s]->hdr->avgdl_ref, p->nleaves) * (float4) mb + others;
}

/* A document that scores through a term with this bound is not above theta, so it is not among the K best that the K-th score theta names */
static bool
bound_settled(float4 bound, float4 theta)
{
	return bound * BOUND_SLACK <= theta;
}

/* The terms of leaf i that segment s holds and the leaf does not read cannot change a result of a search whose K-th score is above theta */
static bool
seg_settled(const SiftPlan *p, int i, int s, float4 theta)
{
	uint8		mb = p->leaves[i].tail_mb[s];

	return mb == 0 || bound_settled(term_bound(p, i, s, mb), theta);
}

bool
sift_plan_tail_settled(const SiftPlan *p, float4 theta)
{
	for (int i = 0; i < p->nleaves; i++)
		for (int s = 0; p->leaves[i].deferred && s < p->nsegs; s++)
			if (!seg_settled(p, i, s, theta))
				return false;
	return true;
}

/*
 * A plan of one wide leaf whose terms left out reach, in some segment, the bound of those it keeps (equal lengths make the bounds tie): a document that scores through a term left
 * out can score what the best document of the terms kept does, and no K-th score of a search of those is out of reach of that (bound_settled wants the bound to clear it by the slack).
 * Its first search could only be repeated on the terms that can, so the leaf is read as all of them at once.
 */
bool
sift_plan_tail_doomed(const SiftPlan *p)
{
	const EvalLeaf *l = &p->leaves[0];

	if (p->nleaves != 1 || !l->deferred)
		return false;
	for (int s = 0; s < p->nsegs; s++)
	{
		uint8		kept = 0;

		for (int i = 0; i < l->nterms[s]; i++)
			kept = Max(kept, l->terms[s][i].info.max_bound);
		if (l->tail_mb[s] > 0 && l->tail_mb[s] >= kept)
			return true;
	}
	return false;
}

/* The smallest bound of a term of leaf i that can lift a document of segment s past theta */
static int
tail_min_bound(const SiftPlan *p, int i, int s, float4 theta)
{
	int			mb = 0;

	while (mb < 255 && bound_settled(term_bound(p, i, s, mb), theta))
		mb++;
	return mb;
}

/*
 * Leaf i in segment s reads the terms that can lift a document past theta: when there are few of them as they are (the direct terms of a leaf, one cursor each, so a
 * document that has none of them is never decoded), else as the one union of those. Every term with a bound below is settled by it and not read.
 */
static void
widen_seg(SiftPlan *p, int i, int s, float4 theta, bool direct)
{
	EvalLeaf   *l = &p->leaves[i];
	int			bound = tail_min_bound(p, i, s, theta);
	uint8		left_out = 0;

	leaf_release(l, s);
	if (direct && theta > -INFINITY && !expand_tail(p, l, s, WIDE_TOP_MAX, bound, &left_out))
		TAIL_STAT(tail_direct);
	else
	{
		leaf_release(l, s);
		union_seg(p, l, s, bound, &left_out);
		TAIL_STAT(tail_unioned);
	}
	for (int j = 0; j < l->nterms[s]; j++)
		l->terms[s][j].w = l->wide_w;
	l->tail_mb[s] = left_out;
	if (left_out > 0)
		TAIL_STAT(tail_trimmed);
}

void
sift_plan_widen(SiftPlan *p, float4 theta)
{
	for (int i = 0; i < p->nleaves; i++)
		for (int s = 0; p->leaves[i].deferred && s < p->nsegs; s++)
			if (!seg_settled(p, i, s, theta))
				widen_seg(p, i, s, theta, true);
}

/*
 * A leaf weighed from the memo (EvalLeaf.warm) is read, for a search of the K hits the memo has its K-th score for, as the terms that reach that score: the search ends
 * on the score it finds, and a score below the memo's widens the leaf again (sift_plan_tail_settled); for any other K the top terms are chosen as a bind would have.
 * True when the plan was widened here.
 */
bool
sift_plan_warm_begin(SiftPlan *p, int64 k)
{
	bool		widened = false;

	for (int i = 0; i < p->nleaves; i++)
	{
		EvalLeaf   *l = &p->leaves[i];

		if (!l->warm)
			continue;
		if (k <= 0 || k > l->warm_k)
			sift_plan_install_top(p);
		else
		{
			l->warm = false;
			for (int s = 0; s < p->nsegs; s++)
				widen_seg(p, i, s, l->warm_theta, false);
			widened = true;
		}
	}
	return widened;
}

/* The K-th score of a search that ended on a plan of one deferred wide leaf, for the next statement (bm25/wmemo.c) */
void
sift_plan_remember_theta(const SiftPlan *p, int64 k, float4 theta)
{
	if (p->nleaves == 1 && p->leaves[0].deferred && theta > -INFINITY)
		sift_wide_memo_hint(p, &p->leaves[0], k, theta);
}
