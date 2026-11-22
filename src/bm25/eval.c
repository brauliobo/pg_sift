/*
 * bm25/eval.c - binds SiftQL queries to an index snapshot and evaluates them
 * over the segments' codec cursors.
 *
 * Bind relaxes each query to something the bitmaps can answer exactly or as
 * a superset (phrases), resolves every leaf to dictionary terms per segment
 * and keeps the term infos. The iterator builds a codec cursor tree per
 * segment (OR of a leaf's terms, AND of positives, ANDNOT of the negatives)
 * and merges the segments' superblock groups into heap-ordered pages. The
 * ranker walks the same trees in score-bound order and the count reuses the
 * iterator.
 */
#include "core/host.h"

#include <math.h>

#include "bm25/analyze.h"
#include "bm25/eval.h"
#include "bm25/eval_int.h"
#include "bm25/prefix.h"
#include "bm25/segment.h"

static EvalNode nothing = {EN_NOTHING, .phrase = -1};

SiftIndexState *
sift_state_open_dir(Relation index, MemoryContext mcxt, const SiftDirSnapshot *dir)
{
	MemoryContext old = MemoryContextSwitchTo(mcxt);
	SiftIndexState *st = palloc0(sizeof(SiftIndexState));
	SiftAnalyzerConfig cfg;

	st->index = index;
	st->mcxt = mcxt;
	if (dir)
		st->dir = sift_dir_copy(dir);
	else
		st->dir = sift_dir_read(index);
	sift_analyzer_config_from_index(index, &cfg);
	if (sift_dir_layout_differs(st->dir, cfg.hash))
		sift_lane_layout_error(index, "analyzer options");
	st->analyzer = sift_analyzer_create(&cfg, mcxt);
	st->priv = palloc0(Max(st->dir->nsegs, 1) * sizeof(SiftSegment *));
	MemoryContextSwitchTo(old);
	return st;
}

SiftIndexState *
sift_state_open(Relation index, MemoryContext mcxt)
{
	return sift_state_open_dir(index, mcxt, NULL);
}

void
sift_state_close(SiftIndexState *st)
{
	SiftSegment **segs = st->priv;

	for (int i = 0; i < st->dir->nsegs; i++)
		if (segs[i])
			sift_seg_close(segs[i]);
}

SiftQuery *
sift_state_query(SiftIndexState *st, const char *q, int len)
{
	return sift_query_analyze(sift_query_parse(q, len), st->analyzer);
}

SiftQuery *
sift_state_query_any(SiftIndexState *st, text *const *texts, int n)
{
	const char **srcs = palloc(n * sizeof(char *));
	int		   *lens = palloc(n * sizeof(int));

	for (int i = 0; i < n; i++)
	{
		srcs[i] = VARDATA_ANY(texts[i]);
		lens[i] = VARSIZE_ANY_EXHDR(texts[i]);
	}
	return sift_query_analyze(sift_query_parse_any(srcs, lens, n), st->analyzer);
}

SiftQuery *
sift_state_query_src(SiftIndexState *st, const SiftQuerySrc *src)
{
	if (src->any)
		return sift_state_query_any(st, src->texts, src->ntexts);
	return sift_state_query(st, VARDATA_ANY(src->texts[0]), VARSIZE_ANY_EXHDR(src->texts[0]));
}

static SiftSegment *
state_segment(SiftIndexState *st, int i)
{
	SiftSegment **segs = st->priv;

	if (segs[i] == NULL)
	{
		segs[i] = sift_seg_open(st->index, &st->dir->segs[i], st->mcxt);
		sift_sr_release(&segs[i]->sr);
	}
	return segs[i];
}

static EvalNode *
en_make(SiftPlan *p, EvalKind kind, int nchild, EvalNode **child)
{
	EvalNode   *n;

	if (nchild == 1 && kind != EN_NOT)
		return child[0];
	n = MemoryContextAlloc(p->mcxt, sizeof(EvalNode));
	n->kind = kind;
	n->leaf = -1;
	n->nchild = nchild;
	n->child = child;
	n->phrase = -1;
	return n;
}

/* Where a node sits: under a NOT (negative) and on a path of ANDs from the root (required) */
typedef struct Place
{
	bool		positive;
	bool		required;
} Place;

static EvalNode *
en_leaf(SiftPlan *p, const SiftQNode *node, Place at, const SiftQNode *phrase)
{
	EvalNode   *n = en_make(p, EN_LEAF, 0, NULL);

	if (p->nleaves == p->maxleaves)
	{
		p->maxleaves = Max(8, 2 * p->maxleaves);
		SIFT_REGROW_IN(p->mcxt, p->leaves, p->maxleaves * sizeof(EvalLeaf));
	}
	p->leaves[p->nleaves] = (EvalLeaf) {.node = node,.scoring = at.positive,.phrase = phrase,
	.optional = phrase && !at.required};
	p->rescore |= phrase && at.positive && !at.required;
	n->leaf = p->nleaves++;
	return n;
}

static EvalNode *
en_not(SiftPlan *p, EvalNode *child)
{
	EvalNode  **kid = MemoryContextAlloc(p->mcxt, sizeof(EvalNode *));

	kid[0] = child;
	return en_make(p, EN_NOT, 1, kid);
}

static EvalNode *
term_leaf(SiftPlan *p, const SiftQNode *n, const char *text, int len, Place at, const SiftQNode *phrase)
{
	SiftQNode  *t = MemoryContextAllocZero(p->mcxt, sizeof(SiftQNode));

	t->kind = SQ_TERM;
	t->boost = n->boost;
	t->text = (char *) text;
	t->textlen = len;
	return en_leaf(p, t, at, phrase);
}

/*
 * A word of a field, or a prefix of at most prefix_index characters, is one
 * term of the index: [field byte] [SEP] text
 */
static EvalNode *
indexed_leaf(SiftPlan *p, const SiftQNode *n, Place at)
{
	char	   *term = MemoryContextAlloc(p->mcxt, n->textlen + 2);
	int			len = 0;

	if (n->field)
		term[len++] = SIFT_FIELD_BYTE(n->field);
	if (n->kind == SQ_PREFIX)
		term[len++] = SIFT_PREFIX_SEP;
	memcpy(term + len, n->text, n->textlen);
	return term_leaf(p, n, term, len + n->textlen, at, NULL);
}

/* A phrase with more terms than this finds its repeated terms by hash, not by comparing every pair */
#define PHRASE_PAIRWISE_MAX	16

static bool
same_phrase_term(const SiftQNode *n, int i, int j)
{
	return n->termlens[j] == n->termlens[i] && memcmp(n->terms[j], n->terms[i], n->termlens[i]) == 0;
}

/* first[i]: the first term of the phrase equal to term i */
static void
phrase_first_terms(const SiftQNode *n, int *first)
{
	uint32		size = 32,
				mask;
	int		   *slot;

	if (n->nterms <= PHRASE_PAIRWISE_MAX)
	{
		for (int i = 0; i < n->nterms; i++)
			for (first[i] = 0; first[i] < i && !same_phrase_term(n, i, first[i]); first[i]++)
				;
		return;
	}
	while (size < 2 * (uint32) n->nterms)
		size *= 2;
	mask = size - 1;
	slot = palloc0(size * sizeof(int));
	for (int i = 0; i < n->nterms; i++)
	{
		uint32		h = (uint32) sift_term_hash(n->terms[i], n->termlens[i]) & mask;

		while (slot[h] && !same_phrase_term(n, i, slot[h] - 1))
			h = (h + 1) & mask;
		if (slot[h] == 0)
			slot[h] = i + 1;
		first[i] = slot[h] - 1;
	}
	pfree(slot);
}

/*
 * A phrase read as the AND of its distinct terms, each scoring with the
 * phrase's boost
 */
static EvalNode *
relax_phrase(SiftPlan *p, const SiftQNode *n, Place at)
{
	EvalNode  **kids = MemoryContextAlloc(p->mcxt, n->nterms * sizeof(EvalNode *));
	int			nk = 0;
	int		   *leaf = MemoryContextAlloc(p->mcxt, n->nterms * sizeof(int));
	int		   *first = palloc(n->nterms * sizeof(int));
	EvalNode   *and;

	phrase_first_terms(n, first);
	for (int i = 0; i < n->nterms; i++)
	{
		if (first[i] < i)
		{
			leaf[i] = leaf[first[i]];
			continue;
		}
		kids[nk++] = term_leaf(p, n, n->terms[i], n->termlens[i], at, n);
		leaf[i] = kids[nk - 1]->leaf;
	}
	pfree(first);
	if (at.positive)
	{
		if (p->nphr == p->phrcap)
		{
			p->phrcap = Max(4, 2 * p->phrcap);
			SIFT_REGROW_IN(p->mcxt, p->phr, (Size) p->phrcap * sizeof(PhraseCheck));
		}
		p->phr[p->nphr++] = (PhraseCheck) {n->nterms, leaf, NULL, n->offs, at.required};
	}
	and = en_make(p, EN_AND, nk, kids);
	if (at.positive && !at.required)
		and->phrase = p->nphr - 1;
	return and;
}

/*
 * The tree the bitmaps evaluate: over = true yields a superset of the
 * matches (positive phrases become ANDs of their terms), false a subset (a
 * phrase matches nothing). NOT flips the direction, so what a NOT excludes
 * is never more than the exact exclusion.
 */
static EvalNode *
relax(SiftPlan *p, const SiftQNode *n, bool over, Place at)
{
	EvalNode  **kids;
	int			nk = 0;

	switch (n->kind)
	{
		case SQ_NOTHING:
			return &nothing;
		case SQ_PHRASE:
			p->recheck |= !(at.required && at.positive);
			p->hardrecheck |= !at.positive;
			return over ? relax_phrase(p, n, at) : &nothing;
		case SQ_AND:
		case SQ_OR:
			kids = MemoryContextAlloc(p->mcxt, n->nchild * sizeof(EvalNode *));
			for (int i = 0; i < n->nchild; i++)
			{
				const SiftQNode *c = n->child[i];
				bool		neg = c->kind == SQ_NOT;
				Place		cat = neg ? (Place) {false, false} : (Place) {at.positive, at.required && n->kind == SQ_AND};
				EvalNode   *e = relax(p, neg ? c->child[0] : c, neg ? !over : over, cat);

				if (e->kind == EN_NOTHING && !neg && n->kind == SQ_AND)
					return &nothing;
				if (e->kind != EN_NOTHING)
					kids[nk++] = neg ? en_not(p, e) : e;
			}
			return nk == 0 ? &nothing : en_make(p, n->kind == SQ_AND ? EN_AND : EN_OR, nk, kids);
		case SQ_TERM:
		case SQ_PREFIX:
			if (n->kind == SQ_TERM ? n->field : n->gram)
				return indexed_leaf(p, n, at);
			return en_leaf(p, n, at, NULL);
		default:
			return en_leaf(p, n, at, NULL);
	}
}

/*
 * The matches the bitmaps decide without the text: the same relaxation read from below, where a phrase the
 * index cannot decide matches nothing and a NOT excludes the superset of what it names. They are a subset
 * of the matches, so a page's other candidates are the only ones to recheck. NULL when no query has any.
 */
static EvalNode *
sure_tree(SiftPlan *p)
{
	EvalNode  **roots = MemoryContextAlloc(p->mcxt, p->nqueries * sizeof(EvalNode *));
	bool		rescore = p->rescore,
				any = true;

	for (int i = 0; i < p->nqueries; i++)
	{
		roots[i] = relax(p, p->queries[i]->root, false, (Place) {true, true});
		any &= roots[i]->kind != EN_NOTHING;
	}
	p->rescore = rescore;
	return any ? en_make(p, EN_AND, p->nqueries, roots) : NULL;
}

typedef struct Expand
{
	SiftPlan   *plan;
	SiftTermMatcher *matcher;
	SiftQKind	kind;			/* of the leaf */
	SiftSegment *seg;
	SiftSegment *live_in;		/* when set, terms without a live posting in it are skipped */
	LeafTerm   *terms;
	int			n;
	int			cap;
	bool		over;
	int			limit;			/* terms kept before the leaf is over */
	int			min_bound;		/* terms with a smaller bound are left out */
	uint8		left_out;		/* the largest bound of those */
	double		tail_none;		/* a capped prefix: what the terms past the limit add to the estimate */
	double		tail_postings;
	double		tail_terms;
} Expand;

static void
expand_add(Expand *e, const char *term, int len, const SiftTermInfo *info)
{
	LeafTerm   *t;

	if (e->n == e->cap)
	{
		e->cap = Max(4, 2 * e->cap);
		SIFT_REGROW_IN(e->plan->mcxt, e->terms, e->cap * sizeof(LeafTerm));
	}
	t = &e->terms[e->n++];
	*t = (LeafTerm) {.len = len,.info = *info};
	t->term = MemoryContextAlloc(e->plan->mcxt, len);
	memcpy(t->term, term, len);
	if (info->flags & SIFT_TERM_INLINE && info->pos_len > 0)
		t->info.inline_pos = memcpy(MemoryContextAlloc(e->plan->mcxt, info->pos_len), info->inline_pos, info->pos_len);
}

static bool
expand_skip(void *arg, const char *term, int len, const char **next, int *nextlen)
{
	return sift_matcher_skip(((Expand *) arg)->matcher, term, len, next, nextlen);
}

static bool
expand_visit(void *arg, const char *term, int len, const SiftTermInfo *info)
{
	Expand	   *e = arg;
	SiftTermInfo live = *info;

	if (!sift_matcher_match(e->matcher, term, len) || (e->live_in && !sift_seg_term_live(e->live_in, info)))
		return true;
	sift_seg_term_stats(e->seg, &live);
	if (live.max_bound < e->min_bound)
	{
		e->left_out = Max(e->left_out, live.max_bound);
		return true;
	}
	if (e->n == e->limit)
	{
		e->over = true;
		/* an estimate of a prefix reads on, from the dictionary only: the terms it keeps are the first ones, not the typical ones */
		if (!e->plan->capped || e->kind != SQ_PREFIX)
			return false;
		e->tail_none *= 1.0 - Min(1.0, live.ldf / Max((double) e->plan->stats.ndocs, 1.0));
		e->tail_postings += live.ldf;
		e->tail_terms++;
		return true;
	}
	expand_add(e, term, len, &live);
	return true;
}

/* Terms across segments are distinct only once */
static int
distinct_terms(const SiftPlan *p, const EvalLeaf *l)
{
	LeafTerm  **all;
	int			n = 0,
				distinct = 0;

	for (int s = 0; s < p->nsegs; s++)
		n += l->nterms[s];
	if (p->nsegs == 1 || n <= sift_max_expansions)
		return n;
	all = palloc(n * sizeof(LeafTerm *));
	n = 0;
	for (int s = 0; s < p->nsegs; s++)
		for (int i = 0; i < l->nterms[s]; i++)
			all[n++] = &l->terms[s][i];
	qsort(all, n, sizeof(LeafTerm *), leaf_term_cmp);
	for (int i = 0; i < n; i++)
		distinct += i == 0 || leaf_term_cmp(&all[i - 1], &all[i]) != 0;
	pfree(all);
	return distinct;
}

static bool
segment_dirty(const SiftPlan *p, int s)
{
	return (p->seg[s]->entry.flags & SIFT_SEG_DIRTY) != 0;
}

/* The leaf's terms in segment s with a bound of at least min_bound; true when they exceed limit */
static bool
expand_bounded(SiftPlan *p, EvalLeaf *l, int s, bool live_only, int limit, int min_bound, uint8 *left_out)
{
	SiftSegment *seg = p->seg[s];
	Expand		e = {.plan = p, .matcher = l->m, .kind = l->node->kind, .seg = seg, .live_in = live_only ? seg : NULL,
					 .limit = limit, .min_bound = min_bound, .tail_none = 1.0};
	SiftTermInfo info;

	if (l->m == NULL)
	{
		if (sift_seg_lookup(seg, l->node->text, l->node->textlen, &info))
			expand_add(&e, l->node->text, l->node->textlen, &info);
	}
	else
	{
		const char *lo,
				   *hi;
		int			lolen,
					hilen;

		sift_matcher_bounds(l->m, &lo, &lolen, &hi, &hilen);
		sift_dict_scan_skipping(seg->dict, lo, lolen, hi, hilen, sift_matcher_skips(l->m) ? expand_skip : NULL, expand_visit, &e);
	}
	sift_sr_release(&seg->sr);
	l->nterms[s] = e.n;
	l->terms[s] = e.terms;
	l->tail_none *= e.tail_none;
	l->tail_postings += e.tail_postings;
	l->tail_terms += e.tail_terms;
	if (left_out)
		*left_out = e.left_out;
	return e.over;
}

/* The leaf's terms in segment s; true when they exceed sift.max_expansions */
static bool
expand_segment(SiftPlan *p, EvalLeaf *l, int s, bool live_only, int limit)
{
	return expand_bounded(p, l, s, live_only, limit, 0, NULL);
}

bool
expand_tail(SiftPlan *p, EvalLeaf *l, int s, int limit, int min_bound, uint8 *left_out)
{
	return expand_bounded(p, l, s, segment_dirty(p, s), limit, min_bound, left_out);
}

/* Expands again without the terms of vacuumed docs; false when no segment has any */
static bool
expand_live(SiftPlan *p, EvalLeaf *l)
{
	bool		any = false;

	for (int s = 0; s < p->nsegs; s++)
		if (segment_dirty(p, s))
		{
			expand_segment(p, l, s, true, sift_max_expansions);
			any = true;
		}
	return any;
}

/*
 * A dictionary keeps the terms of vacuumed docs until its segment is
 * rewritten. They match nothing, so the limit applies to the terms with a
 * live posting; they are only told apart once the limit is reached, which
 * keeps the common expansion free of posting reads. A leaf over the limit is
 * wide.
 */
static void
resolve_leaf(SiftPlan *p, EvalLeaf *l)
{
	const SiftQNode *node = l->node;
	bool		over = false;

	l->m = node->kind == SQ_TERM ? NULL : sift_matcher_create(node);
	if (l->m == NULL)
		l->hash = sift_term_hash(node->text, node->textlen);
	l->nterms = MemoryContextAllocZero(p->mcxt, p->nsegs * sizeof(int));
	l->terms = MemoryContextAllocZero(p->mcxt, p->nsegs * sizeof(LeafTerm *));
	l->tail_none = 1.0;
	for (int s = 0; s < p->nsegs && !over; s++)
		over = expand_segment(p, l, s, false, sift_max_expansions) && !p->capped &&
			   (!segment_dirty(p, s) || expand_segment(p, l, s, true, sift_max_expansions));
	over |= !p->capped && distinct_terms(p, l) > sift_max_expansions &&
		(!expand_live(p, l) || distinct_terms(p, l) > sift_max_expansions);
	if (over && p->scoring && !p->recheck && p->nphr == 0 && l->scoring && l->phrase == NULL)
		tail_defer_leaf(p, l);
	else if (over)
		tail_widen_leaf(p, l);
}

/*
 * The live docs and tokens of the immutable segments, which every participant of a parallel scan takes from the one that
 * exported the bind: a VACUUM running meanwhile must not give the workers other statistics than the leader's
 */
static void
bind_segment_totals(SiftPlan *p, const void *shared)
{
	if (shared)
	{
		const SharedBind *h = shared;

		p->seg_ndocs = h->seg_ndocs;
		p->seg_sum_dl = h->seg_sum_dl;
	}
	else
		for (int s = 0; s < p->nsegs; s++)
		{
			uint64		n,
						sum;

			sift_seg_live_totals(p->seg[s], &n, &sum);
			p->seg_ndocs += n;
			p->seg_sum_dl += sum;
		}
	p->stats.ndocs = p->seg_ndocs;
	p->stats.avgdl = p->seg_sum_dl;
}

typedef struct PhraseOrder
{
	const uint64 *df;
	const int  *leaf;
} PhraseOrder;

/* Terms by ascending df of their leaf, equal dfs by position in the phrase */
static int
phrase_order_cmp(const void *a, const void *b, void *arg)
{
	const PhraseOrder *po = arg;
	int			x = *(const int *) a,
				y = *(const int *) b;
	uint64		dx = po->df[po->leaf[x]],
				dy = po->df[po->leaf[y]];

	if (dx != dy)
		return dx < dy ? -1 : 1;
	return x < y ? -1 : x > y;
}

/* Orders the terms of every phrase by df over the segments and finds where the positions decide */
static void
phrase_prepare(SiftPlan *p)
{
	uint64	   *df = MemoryContextAllocZero(p->mcxt, Max(p->nleaves, 1) * sizeof(uint64));
	bool	   *seen = palloc0(Max(p->nleaves, 1) * sizeof(bool));

	p->heap = p->scoring ? p->recheck : p->hardrecheck;
	if (p->nphr == 0)
	{
		pfree(seen);
		return;
	}
	p->pleaf = MemoryContextAlloc(p->mcxt, p->nleaves * sizeof(int));
	for (int i = 0; i < p->nphr; i++)
		for (int j = 0; j < p->phr[i].nterms; j++)
		{
			int			leaf = p->phr[i].leaf[j];

			for (int s = 0; df[leaf] == 0 && s < p->nsegs; s++)
				df[leaf] += p->leaves[leaf].nterms[s] ? p->leaves[leaf].terms[s][0].info.df : 0;
			if (!seen[leaf] && p->phr[i].required)
				p->pleaf[p->npleaf++] = leaf;
			seen[leaf] |= p->phr[i].required;
		}
	p->phok = MemoryContextAlloc(p->mcxt, Max(p->nsegs * p->nphr, 1) * sizeof(bool));
	p->segheap = MemoryContextAllocZero(p->mcxt, Max(p->nsegs, 1) * sizeof(bool));
	for (int i = 0; i < p->nphr; i++)
	{
		PhraseCheck *ph = &p->phr[i];

		ph->order = MemoryContextAlloc(p->mcxt, ph->nterms * sizeof(int));
		for (int j = 0; j < ph->nterms; j++)
		{
			int			k = j;

			if (ph->nterms > PHRASE_PAIRWISE_MAX)
			{
				ph->order[j] = j;
				continue;
			}
			for (; k > 0 && df[ph->leaf[ph->order[k - 1]]] > df[ph->leaf[j]]; k--)
				ph->order[k] = ph->order[k - 1];
			ph->order[k] = j;
		}
		if (ph->nterms > PHRASE_PAIRWISE_MAX)
		{
			PhraseOrder po = {df, ph->leaf};

			qsort_arg(ph->order, ph->nterms, sizeof(int), phrase_order_cmp, &po);
		}
		for (int s = 0; s < p->nsegs; s++)
		{
			bool		ok = true;

			for (int j = 0; j < ph->nterms; j++)
			{
				const EvalLeaf *l = &p->leaves[ph->leaf[j]];

				ok &= l->nterms[s] == 0 || (l->terms[s][0].info.flags & SIFT_TERM_POS) != 0;
			}
			p->phok[s * p->nphr + i] = ok;
			p->segheap[s] |= !ok;
		}
	}
	p->heap |= p->nmhit > 0;
	for (int s = 0; s < p->nsegs; s++)
		p->heap |= p->segheap[s];
	pfree(seen);
}

static SiftPlan *
plan_bind(SiftIndexState *st, SiftQuery **queries, int nqueries, int flags, const void *shared)
{
	SiftPlan   *p = MemoryContextAllocZero(st->mcxt, sizeof(SiftPlan));
	EvalNode  **roots = MemoryContextAlloc(st->mcxt, nqueries * sizeof(EvalNode *));

	p->st = st;
	p->queries = queries;
	p->nqueries = nqueries;
	p->mcxt = st->mcxt;
	p->segno = MemoryContextAlloc(st->mcxt, Max(st->dir->nsegs, 1) * sizeof(int));
	p->wbufno = MemoryContextAlloc(st->mcxt, Max(st->dir->nsegs, 1) * sizeof(int));
	for (int i = 0; i < st->dir->nsegs; i++)
	{
		if (st->dir->segs[i].kind != SIFT_SEG_IMMUTABLE)
		{
			p->wbufno[p->nwbuf++] = i;
			continue;
		}
		p->segno[p->nsegs++] = i;
	}
	p->stats.nwbuf = p->nwbuf;
	p->capped = (flags & SIFT_BIND_CAPPED) != 0;
	p->scoring = (flags & SIFT_BIND_SCORE) != 0;
	p->seg = MemoryContextAlloc(st->mcxt, Max(p->nsegs, 1) * sizeof(SiftSegment *));
	for (int s = 0; s < p->nsegs; s++)
		p->seg[s] = state_segment(st, p->segno[s]);
	bind_segment_totals(p, shared);

	for (int i = 0; i < nqueries; i++)
	{
		roots[i] = relax(p, queries[i]->root, true, (Place) {true, true});
		p->recheck |= p->rescore;
		if (roots[i]->kind == EN_NOTHING)
		{
			p->root = roots[i];
			bind_stats(p, 0, 0);
			return p;
		}
	}
	p->root = en_make(p, EN_AND, nqueries, roots);
	if (p->recheck && !p->scoring)
		p->sure = sure_tree(p);
	for (int i = 0; i < p->nleaves; i++)
		resolve_leaf(p, &p->leaves[i]);
	if (shared)
		import_wbuf(p, shared);
	else
		bind_wbuf(p);
	phrase_prepare(p);
	if (p->scoring)
	{
		bind_scoring(p);
		if (!shared)
			score_wbuf(p);
	}
	return p;
}

SiftPlan *
sift_plan_bind(SiftIndexState *st, SiftQuery **queries, int nqueries, int flags)
{
	return plan_bind(st, queries, nqueries, flags, NULL);
}

SiftPlan *
sift_plan_bind_shared(SiftIndexState *st, SiftQuery **queries, int nqueries, int flags, const void *shared)
{
	return plan_bind(st, queries, nqueries, flags, shared);
}

/*
 * Reads the term blobs of every leaf in every segment as one plan, before the cursors open them one by one: the segments of an index share
 * its relation, so what a cold query asks of the device is one wave of reads where each (segment, term) was a wait of its own. Blobs a
 * query kept in memory, inline terms and the terms of a wide prefix (already in memory) are left out.
 */
void
sift_plan_prefetch_blobs(const SiftPlan *plan)
{
	SiftMultiRange *r;
	int			n = 0,
				cap = 0;

	for (int i = 0; i < plan->nleaves; i++)
		for (int s = 0; s < plan->nsegs; s++)
			cap += plan->leaves[i].nterms[s];
	if (cap < 2)
		return;
	r = palloc(cap * sizeof(SiftMultiRange));
	for (int i = 0; i < plan->nleaves; i++)
		for (int s = 0; s < plan->nsegs; s++)
			for (int j = 0; j < plan->leaves[i].nterms[s]; j++)
			{
				const LeafTerm *t = &plan->leaves[i].terms[s][j];
				uint64		off,
							len;

				if (t->blob == NULL && sift_post_blob_range(&t->info, &off, &len) && !sift_post_blob_cached(&plan->seg[s]->sr, &t->info))
					r[n++] = (SiftMultiRange) {&plan->seg[s]->sr, off, len};
			}
	if (n > 1)
		sift_sr_prefetch_multi(r, n);
	pfree(r);
}

const SiftPlanStats *
sift_plan_stats(const SiftPlan *plan)
{
	return &plan->stats;
}

void
sift_plan_set_filter(SiftPlan *plan, const SiftFilter *filter)
{
	plan->filter = filter;
}

void
sift_plan_set_tidbridge(SiftPlan *plan, const SiftTidBridge *bridge)
{
	Assert(bridge == NULL || bridge->finalized);
	plan->bridge = bridge;
}

void
sift_plan_set_attrs(SiftPlan *plan, const SiftAttrTest *tests, int n)
{
	plan->attrs = tests;
	plan->nattrs = n;
}

typedef struct Estimate
{
	double		postings;
	double		terms;
	double		range_terms;
} Estimate;

static double
leaf_prob(const SiftPlan *p, const EvalLeaf *l, Estimate *est)
{
	double		none = 1.0,
				ndocs = Max(p->stats.ndocs, 1);

	for (int s = 0; s < p->nsegs; s++)
	{
		for (int i = 0; i < l->nterms[s]; i++)
		{
			none *= 1.0 - Min(1.0, l->terms[s][i].info.ldf / ndocs);
			est->postings += l->terms[s][i].info.ldf;
			est->terms++;
		}
		est->range_terms += Max(l->nterms[s] - 1, 0);
	}
	est->postings += l->tail_postings;
	est->terms += l->tail_terms;
	est->range_terms += l->tail_terms;
	return 1.0 - none * l->tail_none;
}

static double
node_prob(const SiftPlan *p, const EvalNode *n, Estimate *est)
{
	double		prob = n->kind == EN_AND ? 1.0 : 0.0;

	switch (n->kind)
	{
		case EN_NOTHING:
			return 0.0;
		case EN_LEAF:
			return leaf_prob(p, &p->leaves[n->leaf], est);
		case EN_NOT:
			return 1.0 - node_prob(p, n->child[0], est);
		case EN_AND:
			for (int i = 0; i < n->nchild; i++)
				prob *= node_prob(p, n->child[i], est);
			return prob;
		case EN_OR:
			for (int i = 0; i < n->nchild; i++)
				prob = 1.0 - (1.0 - prob) * (1.0 - node_prob(p, n->child[i], est));
			return prob;
	}
	return 0.0;
}

uint32
sift_plan_dir_version(const SiftPlan *p)
{
	return p->st->dir->version;
}

double
sift_plan_docs(const SiftPlan *p)
{
	Estimate	e = {0};

	return node_prob(p, p->root, &e) * p->stats.ndocs;
}

void
sift_eval_estimate_src(Relation index, const SiftQuerySrc *srcs, int nqueries, SiftEstimate *est)
{
	MemoryContext cx = AllocSetContextCreate(CurrentMemoryContext, "sift estimate", ALLOCSET_DEFAULT_SIZES);
	MemoryContext old = MemoryContextSwitchTo(cx);
	SiftIndexState *st = sift_state_open(index, cx);
	SiftQuery **q = palloc(nqueries * sizeof(SiftQuery *));
	SiftPlan   *p;
	Estimate	e = {0};

	for (int i = 0; i < nqueries; i++)
		q[i] = sift_state_query_src(st, &srcs[i]);
	p = sift_plan_bind(st, q, nqueries, SIFT_BIND_CAPPED);
	est->docs = node_prob(p, p->root, &e) * p->stats.ndocs;
	est->postings = e.postings;
	est->terms = e.terms;
	est->range_terms = e.range_terms;
	est->ndocs = p->stats.ndocs;
	sift_state_close(st);
	MemoryContextSwitchTo(old);
	MemoryContextDelete(cx);
}

void
sift_eval_estimate(Relation index, text **queries, int nqueries, SiftEstimate *est)
{
	SiftQuerySrc *srcs = palloc(nqueries * sizeof(SiftQuerySrc));

	for (int i = 0; i < nqueries; i++)
		srcs[i] = (SiftQuerySrc) {&queries[i], 1, false};
	sift_eval_estimate_src(index, srcs, nqueries, est);
	pfree(srcs);
}
