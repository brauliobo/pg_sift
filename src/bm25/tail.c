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

#include "bm25/eval/eval.h"
#include "bm25/eval/int.h"
#include "bm25/prefix.h"
#include "bm25/segment/segment.h"
#include "bm25/wmemo.h"
#include "bm25/wbuf.h"

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

/* The largest score the scoring leaves give a document of each segment, summed once: the leaves a bound adds to one leaf are that sum without the leaf */
static double *
seg_ub_sums(const SiftPlan *p)
{
	double	   *sum = palloc0(Max(p->nsegs, 1) * sizeof(double));

	for (int s = 0; s < p->nsegs; s++)
	{
		CHECK_FOR_INTERRUPTS();
		for (int j = 0; j < p->nleaves; j++)
			if (p->leaves[j].scoring)
				sum[s] += leaf_ub_max(p, &p->leaves[j], s);
	}
	return sum;
}

/* The largest score the leaves other than i give a document of segment s */
static float4
others_ub(const SiftPlan *p, int i, int s, const double *sum)
{
	return (float4) (sum[s] - (p->leaves[i].scoring ? leaf_ub_max(p, &p->leaves[i], s) : 0));
}

/* The largest score a term of bound mb of leaf i gives a document of segment s, and the other leaves give it at most */
static float4
term_bound(const SiftPlan *p, int i, int s, int mb, float4 others)
{
	return sift_kern_ub_weight(p->leaves[i].wide_w, p->stats.avgdl, p->seg[s]->hdr->avgdl_ref, p->nleaves) * (float4) mb + others;
}

/* A document that scores through a term with this bound is not above theta, so it is not among the K best that the K-th score theta names */
static bool
bound_settled(float4 bound, float4 theta)
{
	return bound * BOUND_SLACK <= theta;
}

/* The terms of leaf i that segment s holds and the leaf does not read cannot change a result of a search whose K-th score is above theta */
static bool
seg_settled(const SiftPlan *p, int i, int s, float4 theta, const double *sum)
{
	uint8		mb = p->leaves[i].tail_mb[s];

	return mb == 0 || bound_settled(term_bound(p, i, s, mb, others_ub(p, i, s, sum)), theta);
}

bool
sift_plan_tail_settled(const SiftPlan *p, float4 theta)
{
	double	   *sum;
	bool		settled = true;

	if (p->ndeferred == 0)
		return true;
	sum = seg_ub_sums(p);
	for (int i = 0; settled && i < p->nleaves; i++)
		for (int s = 0; settled && p->leaves[i].deferred && s < p->nsegs; s++)
			settled = seg_settled(p, i, s, theta, sum);
	pfree(sum);
	return settled;
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

/*
 * The smallest bound of a term of leaf i that can lift a document of segment s past theta: the first mb whose term_bound is not settled by
 * theta, 255 when none below is. The bound grows with mb, so the root of the linear bound is within a step or two of it: the steps that
 * remain are taken on the float4 bounds themselves, and the answer is that of testing every mb.
 */
static int
tail_min_bound(const SiftPlan *p, int i, int s, float4 theta, float4 others)
{
	float4		ubw = sift_kern_ub_weight(p->leaves[i].wide_w, p->stats.avgdl, p->seg[s]->hdr->avgdl_ref, p->nleaves);
	double		est;
	int			mb;

	if (!bound_settled(ubw * (float4) 0 + others, theta))
		return 0;
	if (!(ubw > 0))
		return 255;
	est = ((double) theta / (double) BOUND_SLACK - (double) others) / (double) ubw;
	mb = est <= 0 ? 0 : est >= 255 ? 255 : (int) est;
	while (mb > 0 && !bound_settled(ubw * (float4) (mb - 1) + others, theta))
		mb--;
	while (mb < 255 && bound_settled(ubw * (float4) mb + others, theta))
		mb++;
	return mb;
}

/*
 * Leaf i in segment s reads the terms that can lift a document past theta: when there are few of them as they are (the direct terms of a leaf, one cursor each, so a
 * document that has none of them is never decoded), else as the one union of those. Every term with a bound below is settled by it and not read.
 */
static void
widen_seg(SiftPlan *p, int i, int s, float4 theta, bool direct, const double *sum)
{
	EvalLeaf   *l = &p->leaves[i];
	int			bound = tail_min_bound(p, i, s, theta, others_ub(p, i, s, sum));
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
	double	   *sum;

	if (p->ndeferred == 0)
		return;
	sum = seg_ub_sums(p);
	for (int i = 0; i < p->nleaves; i++)
		for (int s = 0; p->leaves[i].deferred && s < p->nsegs; s++)
			if (!seg_settled(p, i, s, theta, sum))
				widen_seg(p, i, s, theta, true, sum);
	pfree(sum);
}

/*
 * A leaf weighed from the memo (EvalLeaf.warm) is read, for a search of the K hits the memo has its K-th score for, as the terms that reach that score: the search ends
 * on the score it finds, and a score below the memo's widens the leaf again (sift_plan_tail_settled); for any other K the top terms are chosen as a bind would have.
 * True when the plan was widened here.
 */
bool
sift_plan_warm_begin(SiftPlan *p, int64 k)
{
	double	   *sum = NULL;
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
			sum = sum ? sum : seg_ub_sums(p);
			for (int s = 0; s < p->nsegs; s++)
				widen_seg(p, i, s, l->warm_theta, false, sum);
			widened = true;
		}
	}
	if (sum)
		pfree(sum);
	return widened;
}

/* The K-th score of a search that ended on a plan of one deferred wide leaf, for the next statement (bm25/wmemo.c) */
void
sift_plan_remember_theta(const SiftPlan *p, int64 k, float4 theta)
{
	if (p->nleaves == 1 && p->leaves[0].deferred && theta > -INFINITY)
		sift_wide_memo_hint(p, &p->leaves[0], k, theta);
}

/* The terms of a scoring leaf that only write buffer docs have weigh like the others: by df over the whole index */
int
tail_wbuf_terms(EvalLeaf *l, LeafTerm **out)
{
	HASH_SEQ_STATUS seq;
	WbufTermEnt *e;
	int			n = 0;

	if (l->mtab == NULL)
		return 0;
	hash_seq_init(&seq, l->mtab);
	while ((e = hash_seq_search(&seq)) != NULL)
	{
		LeafTerm   *t = palloc0(sizeof(LeafTerm));

		t->term = e->term;
		t->len = strlen(e->term);
		t->info.df = t->info.ldf = e->df;
		out[n++] = t;
	}
	return n;
}

/* One source of the terms of a wide leaf in byte order: a segment's dictionary, or the sorted terms of the write buffer docs */
typedef struct TermStream
{
	SiftSegment *seg;			/* NULL: the write buffer */
	SiftDictIter *it;
	LeafTerm  **wb;
	int			nwb;
	int			iwb;
	bool		more;			/* term and ldf hold the next term */
	char		term[SIFT_MAX_TERM_BYTES];
	int			len;
	double		ldf;
	SiftTermInfo info;			/* a segment's term: its dictionary entry, live counts, valid until the stream moves on */
} TermStream;

/* The next live term of the leaf with a posting left in the source; sets more to false at the end */
static void
stream_next(TermStream *ts, const EvalLeaf *l, const char *hi, int hilen)
{
	const char *term;
	int			len;
	SiftTermInfo info;

	ts->more = false;
	if (ts->seg == NULL)
	{
		if (ts->iwb == ts->nwb)
			return;
		ts->len = ts->wb[ts->iwb]->len;
		memcpy(ts->term, ts->wb[ts->iwb]->term, ts->len);
		ts->ldf = ts->wb[ts->iwb++]->info.ldf;
		ts->more = true;
		return;
	}
	while (sift_dict_iter_next(ts->it, &term, &len, &info))
	{
		bool		dirty = (ts->seg->entry.flags & SIFT_SEG_DIRTY) != 0;

		if (hi && sift_term_cmp(term, len, hi, hilen) >= 0)
			return;
		if (!sift_matcher_match(l->m, term, len) || (dirty && !sift_seg_term_live(ts->seg, &info)))
			continue;
		sift_seg_term_stats(ts->seg, &info);
		memcpy(ts->term, term, len);
		ts->len = len;
		ts->info = info;
		ts->ldf = info.ldf;
		ts->more = true;
		return;
	}
}

static bool
top_beats(uint8 mb, double ldf, const char *term, int len, const TopTerm *t)
{
	if (mb != t->mb)
		return mb > t->mb;
	if (ldf != t->ldf)
		return ldf > t->ldf;
	return sift_term_cmp(term, len, t->term, t->len) < 0;
}

static int
top_cmp(const void *a, const void *b)
{
	const TopTerm *x = a,
			   *y = b;

	return sift_term_cmp(x->term, x->len, y->term, y->len);
}

/* A term the leaf does not keep: the segments' bounds of the terms left out (the segments that have it are in present[0, npresent), none: info[s].ldf is 0 where it has not) */
static void
top_left_out(EvalLeaf *l, const SiftTermInfo *info, const int *present, int npresent, int nsegs)
{
	for (int k = 0; k < (present ? npresent : nsegs); k++)
	{
		int			s = present ? present[k] : k;

		if (info[s].ldf > 0)
			l->tail_mb[s] = Max(l->tail_mb[s], info[s].max_bound);
	}
}

static void
top_set(SiftPlan *p, TopTerm *t, const char *term, int len, uint8 mb, double ldf, const SiftTermInfo *info, const int *present, int npresent)
{
	t->term = MemoryContextAlloc(p->mcxt, len);
	memcpy(t->term, term, len);
	t->len = len;
	t->mb = mb;
	t->ldf = ldf;
	t->info = MemoryContextAllocZero(p->mcxt, p->nsegs * sizeof(SiftTermInfo));
	for (int k = 0; k < npresent; k++)
	{
		int			s = present[k];

		t->info[s] = info[s];
		if (info[s].flags & SIFT_TERM_INLINE && info[s].pos_len > 0)
			t->info[s].inline_pos = memcpy(MemoryContextAlloc(p->mcxt, info[s].pos_len), info[s].inline_pos, info[s].pos_len);
	}
}

static void
top_free(SiftPlan *p, TopTerm *t)
{
	for (int s = 0; s < p->nsegs; s++)
		if (t->info[s].flags & SIFT_TERM_INLINE && t->info[s].pos_len > 0)
			pfree((void *) t->info[s].inline_pos);
	pfree(t->info);
	pfree(t->term);
}

/* The kept terms are a binary heap with the worst at the root, so that a term that is not among the best is turned away by one comparison */
static bool
top_better(const TopTerm *a, const TopTerm *b)
{
	return top_beats(a->mb, a->ldf, a->term, a->len, b);
}

static void
top_sift_up(TopTerm *t, int i)
{
	TopTerm		x = t[i];

	for (; i > 0 && top_better(&t[(i - 1) / 2], &x); i = (i - 1) / 2)
		t[i] = t[(i - 1) / 2];
	t[i] = x;
}

static void
top_sift_down(TopTerm *t, int n, int i)
{
	TopTerm		x = t[i];

	for (int c; (c = 2 * i + 1) < n; i = c)
	{
		if (c + 1 < n && top_better(&t[c], &t[c + 1]))
			c++;
		if (!top_better(&x, &t[c]))
			break;
		t[i] = t[c];
	}
	t[i] = x;
}

/* A distinct term of the leaf with a posting in some segment: kept when its bound is among the largest, else left out (present[0, npresent): the segments that have it, ascending, info[s] its entry there) */
static void
top_consider(SiftPlan *p, EvalLeaf *l, TopTerms *tt, const char *term, int len, double ldf, const SiftTermInfo *info, const int *present, int npresent)
{
	uint8		mb = 0;

	for (int k = 0; k < npresent; k++)
	{
		int			s = present[k];

		mb = Max(mb, info[s].max_bound);
		l->all_mb[s] = Max(l->all_mb[s], info[s].max_bound);
	}
	if (npresent == 0)
		return;
	if (tt->n < tt->e)
	{
		top_set(p, &tt->t[tt->n], term, len, mb, ldf, info, present, npresent);
		top_sift_up(tt->t, tt->n++);
	}
	else if (top_beats(mb, ldf, term, len, &tt->t[0]))
	{
		top_left_out(l, tt->t[0].info, NULL, 0, p->nsegs);
		top_free(p, &tt->t[0]);
		top_set(p, &tt->t[0], term, len, mb, ldf, info, present, npresent);
		top_sift_down(tt->t, tt->n, 0);
	}
	else
		top_left_out(l, info, present, npresent, p->nsegs);
}

/* The kept terms as the leaf's terms in each segment, ascending, weighed w */
void
tail_top_install(SiftPlan *p, EvalLeaf *l, TopTerms *tt, float4 w)
{
	qsort(tt->t, tt->n, sizeof(TopTerm), top_cmp);
	for (int s = 0; s < p->nsegs; s++)
	{
		int			n = 0;

		for (int i = 0; i < tt->n; i++)
			n += tt->t[i].info[s].ldf > 0;
		l->terms[s] = n ? MemoryContextAlloc(p->mcxt, n * sizeof(LeafTerm)) : NULL;
		l->nterms[s] = n;
		n = 0;
		for (int i = 0; i < tt->n; i++)
			if (tt->t[i].info[s].ldf > 0)
			{
				LeafTerm   *t = &l->terms[s][n++];

				*t = (LeafTerm) {.len = tt->t[i].len,.info = tt->t[i].info[s],.w = w};
				t->term = memcpy(MemoryContextAlloc(p->mcxt, t->len), tt->t[i].term, t->len);
			}
	}
	for (int i = 0; i < tt->n; i++)
	{
		pfree(tt->t[i].info);
		pfree(tt->t[i].term);
	}
	pfree(tt->t);
}

/* The accumulation of leaf_blended_df over the distinct terms of a wide leaf, in byte order */
typedef struct WideAcc
{
	double		ndocs,
				max_df,
				none,
				first;
	uint64		n;
	bool		saturated;
} WideAcc;

static void
wide_acc(WideAcc *a, double df)
{
	if (a->n++ == 0)
		a->first = df;
	if (a->ndocs <= 0 || a->saturated)
		return;
	if (df >= a->ndocs)
		a->saturated = true;
	else
	{
		a->max_df = Max(a->max_df, df);
		a->none += log(1.0 - df / a->ndocs);
	}
}

static double
wide_result(const WideAcc *a)
{
	if (a->n == 1)
		return a->first;
	if (a->ndocs <= 0)
		return 0;
	if (a->saturated)
		return a->ndocs;
	return Min(Max(a->ndocs * (1.0 - exp(a->none)), a->max_df), a->ndocs);
}

/* The distinct terms of a leaf that a single segment holds and the write buffer does not: the terms of its dictionary as they come */
static void
wide_one_segment(SiftPlan *p, EvalLeaf *l, TopTerms *tt, TermStream *ts, const char *hi, int hilen, WideAcc *a)
{
	static const int present[1] = {0};
	SiftTermInfo info;

	for (; ts->more; stream_next(ts, l, hi, hilen))
	{
		if (ts->ldf <= 0)
			continue;
		info = ts->info;
		if (tt)
			top_consider(p, l, tt, ts->term, ts->len, ts->ldf, &info, present, 1);
		wide_acc(a, ts->ldf);
	}
}

/* Stream a before b: the smaller term, equal terms by stream (the segments in order, then the write buffer) */
static bool
stream_before(const TermStream *ts, int a, int b)
{
	int			c = sift_term_cmp(ts[a].term, ts[a].len, ts[b].term, ts[b].len);

	return c < 0 || (c == 0 && a < b);
}

static void
stream_down(const TermStream *ts, int *heap, int n, int i)
{
	int			x = heap[i];

	for (int c; (c = 2 * i + 1) < n; i = c)
	{
		if (c + 1 < n && stream_before(ts, heap[c + 1], heap[c]))
			c++;
		if (!stream_before(ts, heap[c], x))
			break;
		heap[i] = heap[c];
	}
	heap[i] = x;
}

/*
 * The distinct terms over the segments and the write buffer: the minimum of the streams, the df of it over the streams that hold it, added in
 * stream order. Many streams are a heap: the streams that hold a term pop together and only they are looked at.
 */
static void
wide_merge(SiftPlan *p, EvalLeaf *l, TopTerms *tt, TermStream *ts, const char *hi, int hilen, WideAcc *acc)
{
	int			ns = p->nsegs + 1,
				nheap = 0;
	SiftTermInfo *cur_info = palloc(ns * sizeof(SiftTermInfo));
	int		   *heap = palloc(ns * sizeof(int)),
			   *eq = palloc(ns * sizeof(int)),
			   *present = palloc(ns * sizeof(int));

	for (int i = 0; i < ns; i++)
		if (ts[i].more)
			heap[nheap++] = i;
	for (int i = nheap / 2 - 1; i >= 0; i--)
		stream_down(ts, heap, nheap, i);
	while (nheap > 0)
	{
		double		df = 0;
		char		cur[SIFT_MAX_TERM_BYTES];
		int			curlen = ts[heap[0]].len,
					neq = 0,
					npresent = 0;

		memcpy(cur, ts[heap[0]].term, curlen);
		while (nheap > 0 && sift_term_cmp(ts[heap[0]].term, ts[heap[0]].len, cur, curlen) == 0)
		{
			eq[neq++] = heap[0];
			heap[0] = heap[--nheap];
			stream_down(ts, heap, nheap, 0);
		}
		for (int k = 0; k < neq; k++)
		{
			int			i = eq[k];

			df += ts[i].ldf;
			if (i < p->nsegs && ts[i].ldf > 0)
			{
				present[npresent++] = i;
				cur_info[i] = ts[i].info;
			}
		}
		if (df > 0 && tt)
			top_consider(p, l, tt, cur, curlen, df, cur_info, present, npresent);
		for (int k = 0; k < neq; k++)
		{
			int			i = eq[k];

			stream_next(&ts[i], l, i < p->nsegs ? hi : NULL, hilen);
			if (ts[i].more)
			{
				heap[nheap++] = i;
				for (int c = nheap - 1; c > 0 && stream_before(ts, heap[c], heap[(c - 1) / 2]); c = (c - 1) / 2)
				{
					int			t = heap[c];

					heap[c] = heap[(c - 1) / 2];
					heap[(c - 1) / 2] = t;
				}
			}
		}
		if (df > 0)
			wide_acc(acc, df);
	}
	pfree(cur_info);
	pfree(heap);
	pfree(eq);
	pfree(present);
}

/*
 * The blended df of a wide leaf, from a merge of the dictionaries of the segments and the terms of the write buffer in byte order, one distinct term at a
 * time: the terms are not kept, and the accumulation is that of leaf_blended_df over the distinct terms in the same order.
 */
double
tail_wide_blended_df(SiftPlan *p, EvalLeaf *l, TopTerms *tt)
{
	WideAcc		acc = {.ndocs = p->stats.ndocs};
	TermStream *ts = palloc0((p->nsegs + 1) * sizeof(TermStream));
	LeafTerm  **wb = palloc(Max(l->mtab ? hash_get_num_entries(l->mtab) : 0, 1) * sizeof(LeafTerm *));
	const char *lo,
			   *hi;
	int			lolen,
				hilen;

	sift_matcher_bounds(l->m, &lo, &lolen, &hi, &hilen);
	for (int s = 0; s < p->nsegs; s++)
	{
		ts[s].seg = p->seg[s];
		ts[s].it = sift_dict_iter(p->seg[s]->dict, lo, lolen, CurrentMemoryContext);
		stream_next(&ts[s], l, hi, hilen);
	}
	ts[p->nsegs].wb = wb;
	ts[p->nsegs].nwb = tail_wbuf_terms(l, wb);
	qsort(wb, ts[p->nsegs].nwb, sizeof(LeafTerm *), leaf_term_cmp);
	stream_next(&ts[p->nsegs], l, NULL, 0);
	if (p->nsegs == 1 && ts[1].nwb == 0)
		wide_one_segment(p, l, tt, &ts[0], hi, hilen, &acc);
	else
		wide_merge(p, l, tt, ts, hi, hilen, &acc);
	for (int s = 0; s < p->nsegs; s++)
		sift_sr_release(&p->seg[s]->sr);
	pfree(ts);
	pfree(wb);
	return wide_result(&acc);
}

/* The top terms of a deferred leaf and the blended df, from the dictionaries; the leaf reads no term before this */
double
tail_weigh_top(SiftPlan *p, EvalLeaf *l, TopTerms *tt)
{
	tt->e = Min(Max(sift_max_expansions, 1), WIDE_TOP_MAX);
	tt->t = palloc0(tt->e * sizeof(TopTerm));
	l->tail_mb = MemoryContextAllocZero(p->mcxt, Max(p->nsegs, 1));
	l->all_mb = MemoryContextAllocZero(p->mcxt, Max(p->nsegs, 1));
	return tail_wide_blended_df(p, l, tt);
}

/* The kept terms of the deferred leaf (top_install) once its weight is known */
static void
install_top(SiftPlan *p, EvalLeaf *l, TopTerms *tt)
{
	tail_top_install(p, l, tt, l->wide_w);
	l->warm = false;
}

void
sift_plan_install_top(SiftPlan *p)
{
	for (int i = 0; i < p->nleaves; i++)
	{
		EvalLeaf   *l = &p->leaves[i];
		TopTerms	tt = {0};

		if (l->warm)
		{
			tail_weigh_top(p, l, &tt);
			install_top(p, l, &tt);
		}
	}
}
