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

#include "bm25/eval.h"
#include "bm25/eval_int.h"
#include "bm25/kernels.h"
#include "bm25/wbuf.h"
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
		st->dir = *dir;
	else
		sift_dir_read(index, &st->dir);
	sift_analyzer_config_from_index(index, &cfg);
	if (cfg.hash != st->dir.meta.analyzer_hash)
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("index \"%s\" was built with other analyzer options", RelationGetRelationName(index)),
				 errhint("Use REINDEX to apply the new options.")));
	st->analyzer = sift_analyzer_create(&cfg, mcxt);
	st->opts = sift_index_options(index);
	st->priv = palloc0(Max(st->dir.nsegs, 1) * sizeof(SiftSegment *));
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

	for (int i = 0; i < st->dir.nsegs; i++)
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
		segs[i] = sift_seg_open(st->index, &st->dir.segs[i], st->mcxt);
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
		p->leaves = p->leaves ? repalloc(p->leaves, p->maxleaves * sizeof(EvalLeaf)) :
			MemoryContextAlloc(p->mcxt, p->maxleaves * sizeof(EvalLeaf));
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
	EvalNode   *and;

	for (int i = 0; i < n->nterms; i++)
	{
		int			j;

		for (j = 0; j < i; j++)
			if (n->termlens[j] == n->termlens[i] && memcmp(n->terms[j], n->terms[i], n->termlens[i]) == 0)
				break;
		if (j < i)
		{
			leaf[i] = leaf[j];
			continue;
		}
		kids[nk++] = term_leaf(p, n, n->terms[i], n->termlens[i], at, n);
		leaf[i] = kids[nk - 1]->leaf;
	}
	if (at.positive)
	{
		p->phr = p->phr ? repalloc(p->phr, (p->nphr + 1) * sizeof(PhraseCheck)) :
			MemoryContextAlloc(p->mcxt, sizeof(PhraseCheck));
		p->phr[p->nphr++] = (PhraseCheck) {n->nterms, leaf, NULL, at.required};
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
		e->terms = e->terms ? repalloc(e->terms, e->cap * sizeof(LeafTerm)) :
			MemoryContextAlloc(e->plan->mcxt, e->cap * sizeof(LeafTerm));
	}
	t = &e->terms[e->n++];
	t->term = MemoryContextAlloc(e->plan->mcxt, len);
	memcpy(t->term, term, len);
	t->len = len;
	t->info = *info;
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
	if (e->n == sift_max_expansions)
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

/* The leaf's terms in segment s; true when they exceed sift.max_expansions */
static bool
expand_segment(SiftPlan *p, EvalLeaf *l, int s, bool live_only)
{
	SiftSegment *seg = p->seg[s];
	Expand		e = {.plan = p, .matcher = l->m, .kind = l->node->kind, .seg = seg, .live_in = live_only ? seg : NULL, .tail_none = 1.0};
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
	return e.over;
}

/* Expands again without the terms of vacuumed docs; false when no segment has any */
static bool
expand_live(SiftPlan *p, EvalLeaf *l)
{
	bool		any = false;

	for (int s = 0; s < p->nsegs; s++)
		if (segment_dirty(p, s))
		{
			expand_segment(p, l, s, true);
			any = true;
		}
	return any;
}

/*
 * The term that indexed_leaf makes of a prefix of the leaf's field; the name of any other wide leaf is one
 * byte no word starts with, and its cache key (NULL when too long to be one) tells it from the others
 */
static void
wide_names(SiftPlan *p, EvalLeaf *l, char **ckey, int *cklen)
{
	const SiftQNode *n = l->node;
	int			len = 0;

	*ckey = MemoryContextAlloc(p->mcxt, n->textlen + 4);
	if (n->kind == SQ_PREFIX)
	{
		if (n->field)
			(*ckey)[len++] = SIFT_FIELD_BYTE(n->field);
		(*ckey)[len++] = SIFT_PREFIX_SEP;
		memcpy(*ckey + len, n->text, n->textlen);
		l->wide = *ckey;
		l->widelen = *cklen = len + n->textlen;
		return;
	}
	l->wide = pstrdup("\xFF");
	l->widelen = 1;
	(*ckey)[len++] = 0xFF;
	(*ckey)[len++] = n->kind;
	(*ckey)[len++] = n->fuzzy;
	(*ckey)[len++] = n->field;
	memcpy(*ckey + len, n->text, n->textlen);
	*cklen = len + n->textlen;
	if (*cklen > SIFT_MAX_TERM_BYTES + 2)
		*ckey = NULL;
}

/*
 * A leaf with more terms than the limit is not an OR of its terms, one weight each: it reads as the one
 * term that holds the documents with a word the leaf matches, tf the number of those words (for a prefix
 * the one term prefix_index would have indexed for it, bm25/prefix.c), in every segment, so that the
 * leaf is weighed alike everywhere.
 */
static void
widen_leaf(SiftPlan *p, EvalLeaf *l)
{
	SiftScoreRef ref = {(float4) p->st->dir.meta.k1, (float4) p->st->dir.meta.b, 0};
	char	   *ckey;
	int			cklen;

	wide_names(p, l, &ckey, &cklen);
	for (int s = 0; s < p->nsegs; s++)
	{
		LeafTerm   *t = MemoryContextAllocZero(p->mcxt, sizeof(LeafTerm));

		for (int i = 0; i < l->nterms[s]; i++)
			pfree(l->terms[s][i].term);
		if (l->terms[s])
			pfree(l->terms[s]);
		ref.avgdl = p->seg[s]->hdr->avgdl_ref;
		t->blob = sift_prefix_union(p->seg[s], l->m, ckey, cklen, &ref, p->scoring && l->scoring, p->mcxt, &t->info);
		t->term = l->wide;
		t->len = l->widelen;
		l->nterms[s] = t->info.df > 0;
		l->terms[s] = t;
	}
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
		over = expand_segment(p, l, s, false) && !p->capped && (!segment_dirty(p, s) || expand_segment(p, l, s, true));
	over |= !p->capped && distinct_terms(p, l) > sift_max_expansions &&
		(!expand_live(p, l) || distinct_terms(p, l) > sift_max_expansions);
	if (over)
		widen_leaf(p, l);
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

/* Orders the terms of every phrase by df over the segments and finds where the positions decide */
static void
phrase_prepare(SiftPlan *p)
{
	uint64	   *df = MemoryContextAllocZero(p->mcxt, Max(p->nleaves, 1) * sizeof(uint64));

	p->heap = p->scoring ? p->recheck : p->hardrecheck;
	if (p->nphr == 0)
		return;
	p->pleaf = MemoryContextAlloc(p->mcxt, p->nleaves * sizeof(int));
	for (int i = 0; i < p->nphr; i++)
		for (int j = 0; j < p->phr[i].nterms; j++)
		{
			int			leaf = p->phr[i].leaf[j],
						k;

			for (int s = 0; df[leaf] == 0 && s < p->nsegs; s++)
				df[leaf] += p->leaves[leaf].nterms[s] ? p->leaves[leaf].terms[s][0].info.df : 0;
			for (k = 0; k < p->npleaf && p->pleaf[k] != leaf; k++)
				;
			if (k == p->npleaf && p->phr[i].required)
				p->pleaf[p->npleaf++] = leaf;
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

			for (; k > 0 && df[ph->leaf[ph->order[k - 1]]] > df[ph->leaf[j]]; k--)
				ph->order[k] = ph->order[k - 1];
			ph->order[k] = j;
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
	p->segno = MemoryContextAlloc(st->mcxt, Max(st->dir.nsegs, 1) * sizeof(int));
	p->wbufno = MemoryContextAlloc(st->mcxt, Max(st->dir.nsegs, 1) * sizeof(int));
	for (int i = 0; i < st->dir.nsegs; i++)
	{
		if (st->dir.segs[i].kind != SIFT_SEG_IMMUTABLE)
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

typedef struct Estimate
{
	double		postings;
	double		terms;
} Estimate;

static double
leaf_prob(const SiftPlan *p, const EvalLeaf *l, Estimate *est)
{
	double		none = 1.0,
				ndocs = Max(p->stats.ndocs, 1);

	for (int s = 0; s < p->nsegs; s++)
		for (int i = 0; i < l->nterms[s]; i++)
		{
			none *= 1.0 - Min(1.0, l->terms[s][i].info.ldf / ndocs);
			est->postings += l->terms[s][i].info.ldf;
			est->terms++;
		}
	est->postings += l->tail_postings;
	est->terms += l->tail_terms;
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
	return p->st->dir.version;
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
