/*
 * sift_eval.c - binds SiftQL queries to an index snapshot and evaluates them
 * over the segments' codec cursors (DESIGN.md (a), (h)).
 *
 * Bind relaxes each query to something the bitmaps can answer exactly or as
 * a superset (phrases), resolves every leaf to dictionary terms per segment
 * and keeps the term infos. The iterator builds a codec cursor tree per
 * segment (OR of a leaf's terms, AND of positives, ANDNOT of the negatives)
 * and merges the segments' superblock groups into heap-ordered pages. The
 * ranker walks the same trees in score-bound order and the count reuses the
 * iterator.
 */
#include "sift_host.h"

#include <math.h>

#include "sift_eval.h"
#include "sift_kernels.h"
#include "sift_mutable.h"
#include "sift_prefix.h"
#include "sift_segment.h"

typedef struct LeafTerm
{
	char	   *term;
	int			len;
	SiftTermInfo info;
	const uint8 *blob;			/* a wide prefix's term: its blob in memory (NULL: inline); else read from the segment */
	float4		w;				/* scoring leaves: (boost * idf) * (k1 + 1), idf of the df over all segments */
} LeafTerm;

/* A query leaf and its dictionary terms in every segment */
typedef struct EvalLeaf
{
	const SiftQNode *node;
	SiftTermMatcher *m;			/* NULL for a plain term */
	uint32		hash;			/* plain term: sift_term_hash */
	bool		scoring;		/* a positive leaf: counts in the score */
	const SiftQNode *phrase;	/* the phrase this leaf is a term of, or NULL */
	bool		optional;		/* phrase leaf that a match need not satisfy */
	char	   *wide;			/* prefix with more terms than sift.max_expansions: the term it reads as (indexed_leaf's) */
	int			widelen;
	double		tail_none;		/* estimate: P(no document has any term of the prefix past the limit) over all segments */
	double		tail_postings;
	double		tail_terms;
	int		   *nterms;			/* [nsegs] */
	LeafTerm  **terms;			/* [nsegs] */
	int			nuniq;			/* distinct terms over all segments, ascending, with their weights */
	LeafTerm  **uniq;
	HTAB	   *mtab;			/* scoring: df of the leaf's terms in the mutable docs (MutTermEnt) */
} EvalLeaf;

/* A term of a scoring leaf found in mutable docs, zero padded */
typedef struct MutTermEnt
{
	char		term[SIFT_MAX_TERM_BYTES + 1];
	uint32		df;
} MutTermEnt;

/* A live matching doc of a mutable segment; scoring plans keep the doc to score it once the weights are known */
typedef struct MutHit
{
	SiftKey		key;
	float4		score;
	SiftMutDoc *doc;
} MutHit;

typedef enum EvalKind
{
	EN_NOTHING,
	EN_LEAF,
	EN_AND,
	EN_OR,
	EN_NOT						/* only as a child of EN_AND */
} EvalKind;

typedef struct EvalNode
{
	EvalKind	kind;
	int			leaf;
	int			nchild;
	struct EvalNode **child;
	int			phrase;			/* the phrase (PhraseCheck) this subtree is the relaxation of, -1: none */
} EvalNode;

/*
 * A phrase the index decides from the positions of its terms where its segments have them: leaf[i] is
 * the leaf of the i-th term, order the terms by ascending df (the rarest term narrows the candidates
 * first). A required phrase (on a path of ANDs from the root) refines the final matches; an optional
 * one (under an OR) is a cursor of its own that yields the docs which hold it.
 */
typedef struct PhraseCheck
{
	int			nterms;
	int		   *leaf;
	int		   *order;
	bool		required;
} PhraseCheck;

struct SiftPlan
{
	SiftIndexState *st;
	MemoryContext mcxt;
	int			nsegs;
	int		   *segno;			/* directory index of each immutable segment */
	SiftSegment **seg;
	int			nmut;
	int		   *mutno;			/* directory index of each mutable segment */
	MutHit	   *mhit;			/* live matches of the mutable segments, ascending */
	int64		nmhit;
	uint64		mut_ndocs;		/* documents and tokens of the mutable segments, deleted ones included */
	uint64		mut_sum_dl;
	int			nleaves;
	int			maxleaves;
	EvalLeaf   *leaves;
	EvalNode   *root;
	EvalNode   *sure;			/* unranked plans with a phrase: the matches the bitmaps decide alone, NULL when none */
	SiftQuery **queries;
	int			nqueries;
	bool		recheck;		/* ranked scans: a phrase that is not required, every candidate is rechecked in the heap */
	bool		hardrecheck;	/* unranked scans: a negated phrase, every candidate is rechecked in the heap */
	PhraseCheck *phr;			/* phrases decided by positions where the segment has them; the heap decides the other candidates */
	int			nphr;
	int		   *pleaf;			/* the distinct leaves of the required phrases */
	int			npleaf;
	bool	   *phok;			/* [nsegs * nphr] the segment holds the positions of all terms of the phrase */
	bool	   *segheap;		/* [nsegs] some phrase is not decided by the segment's positions */
	bool		heap;			/* some candidates need the heap recheck */
	bool		rescore;		/* a phrase the score depends on: the exact score comes from the text */
	bool		capped;			/* estimate: stop expanding at the limit */
	bool		scoring;
	const SiftFilter *filter;	/* ranked search: the tuples worth scoring */
	SiftBm25	bm;
	SiftPlanStats stats;
};

static EvalNode nothing = {EN_NOTHING, .phrase = -1};

/* ---------- state ---------- */

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

/* ---------- bind ---------- */

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
	const SiftTermMatcher *matcher;
	SiftQKind	kind;			/* of the leaf */
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
expand_visit(void *arg, const char *term, int len, const SiftTermInfo *info)
{
	Expand	   *e = arg;

	if (!sift_matcher_match(e->matcher, term, len) || (e->live_in && !sift_seg_term_live(e->live_in, info)))
		return true;
	if (e->n == sift_max_expansions)
	{
		e->over = true;
		/* an estimate of a prefix reads on, from the dictionary only: the terms it keeps are the first ones, not the typical ones */
		if (!e->plan->capped || e->kind != SQ_PREFIX)
			return false;
		e->tail_none *= 1.0 - Min(1.0, info->df / Max((double) e->plan->stats.ndocs, 1.0));
		e->tail_postings += info->df;
		e->tail_terms++;
		return true;
	}
	expand_add(e, term, len, info);
	return true;
}

static int
leaf_term_cmp(const void *a, const void *b)
{
	const LeafTerm *x = *(LeafTerm *const *) a,
			   *y = *(LeafTerm *const *) b;
	int			c = memcmp(x->term, y->term, Min(x->len, y->len));

	return c ? c : x->len - y->len;
}

static void
limit_error(const SiftQNode *leaf)
{
	ereport(ERROR,
			(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
			 errmsg("sift: the leaf at position %d expands to more than %d terms", leaf->loc, sift_max_expansions),
			 errhint("Narrow the pattern or raise sift.max_expansions.")));
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
	Expand		e = {.plan = p, .matcher = l->m, .kind = l->node->kind, .live_in = live_only ? seg : NULL, .tail_none = 1.0};
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
		sift_dict_scan(seg->dict, lo, lolen, hi, hilen, expand_visit, &e);
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

/* The term that indexed_leaf makes of a prefix of the leaf's field */
static int
prefix_term(const SiftQNode *n, char *term)
{
	int			len = 0;

	if (n->field)
		term[len++] = SIFT_FIELD_BYTE(n->field);
	term[len++] = SIFT_PREFIX_SEP;
	memcpy(term + len, n->text, n->textlen);
	return len + n->textlen;
}

/*
 * A prefix with more terms than the limit is not an OR of its terms, one weight each: it reads as the
 * one term prefix_index would have indexed for it (sift_prefix.c), in every segment, so that the
 * leaf is weighed alike everywhere.
 */
static void
widen_leaf(SiftPlan *p, EvalLeaf *l)
{
	const SiftQNode *node = l->node;
	SiftScoreRef ref = {(float4) p->st->dir.meta.k1, (float4) p->st->dir.meta.b, 0};

	l->wide = MemoryContextAlloc(p->mcxt, node->textlen + 2);
	l->widelen = prefix_term(node, l->wide);
	for (int s = 0; s < p->nsegs; s++)
	{
		LeafTerm   *t = MemoryContextAllocZero(p->mcxt, sizeof(LeafTerm));

		for (int i = 0; i < l->nterms[s]; i++)
			pfree(l->terms[s][i].term);
		if (l->terms[s])
			pfree(l->terms[s]);
		ref.avgdl = p->seg[s]->hdr->avgdl_ref;
		t->blob = sift_prefix_union(p->seg[s], l->m, l->wide, l->widelen, &ref, p->scoring && l->scoring, p->mcxt, &t->info);
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
 * keeps the common expansion free of posting reads. A prefix over the limit
 * is wide, any other leaf is an error.
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
	if (!over)
		return;
	if (node->kind != SQ_PREFIX)
		limit_error(node);
	widen_leaf(p, l);
}

/* ---------- mutable segments ---------- */

/*
 * Mutable segments are not indexed by term: their docs are matched one by
 * one against the relaxed tree (pages whose bloom rules the tree out are
 * skipped) and the live matching tids join the iterator's output.
 */

static bool
node_may_match(const SiftPlan *p, const EvalNode *n, const uint64 *bloom)
{
	int			npos = 0;

	switch (n->kind)
	{
		case EN_NOTHING:
			return false;
		case EN_LEAF:
			return p->leaves[n->leaf].m != NULL || sift_mut_bloom_has(bloom, p->leaves[n->leaf].hash);
		case EN_NOT:
			return true;
		case EN_AND:
			for (int i = 0; i < n->nchild; i++)
				if (n->child[i]->kind != EN_NOT)
				{
					npos++;
					if (!node_may_match(p, n->child[i], bloom))
						return false;
				}
			return npos > 0;
		case EN_OR:
			for (int i = 0; i < n->nchild; i++)
				if (node_may_match(p, n->child[i], bloom))
					return true;
			return false;
	}
	return false;
}

static bool
mut_page_ok(void *arg, const uint64 *bloom)
{
	SiftPlan   *p = arg;

	return node_may_match(p, p->root, bloom);
}

/* Pages with a term of a scoring leaf: every doc that has one counts in the document frequencies */
static bool
mut_leaf_ok(void *arg, const uint64 *bloom)
{
	const SiftPlan *p = arg;

	for (int i = 0; i < p->nleaves; i++)
		if (p->leaves[i].scoring && (p->leaves[i].m != NULL || sift_mut_bloom_has(bloom, p->leaves[i].hash)))
			return true;
	return false;
}

static bool
leaf_term_hit(const EvalLeaf *l, const char *t, int len, uint32 hash)
{
	return l->m ? sift_matcher_match(l->m, t, len) :
		(hash == l->hash && len == l->node->textlen && memcmp(t, l->node->text, len) == 0);
}

static bool
leaf_hits(const EvalLeaf *l, const SiftMutDoc *d)
{
	const char *t = d->terms;

	for (int i = 0; i < d->nterms; t += d->len[i++])
		if (leaf_term_hit(l, t, d->len[i], d->hash[i]))
			return true;
	return false;
}

static bool
doc_matches(const SiftPlan *p, const EvalNode *n, const SiftMutDoc *d)
{
	int			npos = 0;

	switch (n->kind)
	{
		case EN_NOTHING:
			return false;
		case EN_LEAF:
			return leaf_hits(&p->leaves[n->leaf], d);
		case EN_NOT:
			return !doc_matches(p, n->child[0], d);
		case EN_AND:
			for (int i = 0; i < n->nchild; i++)
			{
				npos += n->child[i]->kind != EN_NOT;
				if (!doc_matches(p, n->child[i], d))
					return false;
			}
			return npos > 0;
		case EN_OR:
			for (int i = 0; i < n->nchild; i++)
				if (doc_matches(p, n->child[i], d))
					return true;
			return false;
	}
	return false;
}

static int
hit_cmp(const void *a, const void *b)
{
	return pg_cmp_u64(((const MutHit *) a)->key, ((const MutHit *) b)->key);
}

/* The leaf's mutable term of the zero padded key, created with df 0 */
static MutTermEnt *
leaf_mut_term(SiftPlan *p, EvalLeaf *l, const char *key)
{
	bool		found;
	MutTermEnt *e;

	if (l->mtab == NULL)
	{
		HASHCTL		ctl = {.keysize = SIFT_MAX_TERM_BYTES + 1, .entrysize = sizeof(MutTermEnt), .hcxt = p->mcxt};

		l->mtab = hash_create("sift mutable terms", 64, &ctl, HASH_ELEM | HASH_BLOBS | HASH_CONTEXT);
	}
	e = hash_search(l->mtab, key, HASH_ENTER, &found);
	if (!found)
		e->df = 0;
	return e;
}

static void
leaf_mut_count(SiftPlan *p, EvalLeaf *l, const char *t, int len)
{
	char		key[SIFT_MAX_TERM_BYTES + 1] = {0};

	memcpy(key, t, len);
	leaf_mut_term(p, l, key)->df++;
}

/*
 * One more document for every term of a scoring leaf that the doc has, for the wide term of a wide
 * leaf if it has any word of it (deleted docs count like in the statistics)
 */
static void
doc_count_terms(SiftPlan *p, const SiftMutDoc *d)
{
	for (int i = 0; i < p->nleaves; i++)
	{
		EvalLeaf   *l = &p->leaves[i];
		const char *t = d->terms;

		for (int j = 0; l->scoring && j < d->nterms; t += d->len[j++])
		{
			if (!leaf_term_hit(l, t, d->len[j], d->hash[j]))
				continue;
			if (l->wide)
			{
				leaf_mut_count(p, l, l->wide, l->widelen);
				break;
			}
			leaf_mut_count(p, l, t, d->len[j]);
		}
	}
}

static SiftMutDoc *
mutdoc_copy(MemoryContext cx, const SiftMutDoc *d)
{
	size_t		tl = 0;
	SiftMutDoc *c;
	uint32	   *hash;
	uint8	   *tf,
			   *len;

	for (int i = 0; i < d->nterms; i++)
		tl += d->len[i];
	c = MemoryContextAlloc(cx, sizeof(SiftMutDoc) + d->nterms * (sizeof(uint32) + 2) + tl);
	hash = (uint32 *) (c + 1);
	tf = (uint8 *) (hash + d->nterms);
	len = tf + d->nterms;
	*c = *d;
	c->hash = memcpy(hash, d->hash, d->nterms * sizeof(uint32));
	c->tf = memcpy(tf, d->tf, d->nterms);
	c->len = memcpy(len, d->len, d->nterms);
	c->terms = memcpy(len + d->nterms, d->terms, tl);
	return c;
}

/* Statistics count the mutable docs too (deleted ones like the immutable entries do) */
static void
bind_stats(SiftPlan *p, uint64 mut_ndocs, uint64 mut_sum_dl)
{
	double		sum = p->stats.avgdl + mut_sum_dl;

	p->stats.ndocs += mut_ndocs;
	p->stats.avgdl = p->stats.ndocs > 0 ? sum / p->stats.ndocs : 0;
}

static void
bind_mutable(SiftPlan *p)
{
	uint64		ndocs = 0,
				sum_dl = 0;
	int64		cap = 0;

	for (int i = 0; i < p->nmut && !p->capped; i++)
	{
		SiftMutReader *r = sift_mut_open(p->st->index, &p->st->dir.segs[p->mutno[i]], p->mcxt,
										 p->scoring ? mut_leaf_ok : mut_page_ok, p);
		SiftMutDoc	doc;
		uint64		n,
					sum;

		while (sift_mut_next(r, &doc))
		{
			if (p->scoring)
				doc_count_terms(p, &doc);
			if (doc.deleted || !doc_matches(p, p->root, &doc))
				continue;
			if (p->nmhit == cap)
			{
				cap = Max(64, 2 * cap);
				p->mhit = p->mhit ? repalloc_huge(p->mhit, cap * sizeof(MutHit)) :
					MemoryContextAllocHuge(p->mcxt, cap * sizeof(MutHit));
			}
			p->mhit[p->nmhit++] = (MutHit) {sift_key_from_tid(&doc.tid), 0, p->scoring ? mutdoc_copy(p->mcxt, &doc) : NULL};
		}
		sift_mut_totals(r, &n, &sum);
		ndocs += n;
		sum_dl += sum;
		sift_mut_close(r);
	}
	if (p->nmhit > 1)
	{
		int64		k = 1;

		qsort(p->mhit, p->nmhit, sizeof(MutHit), hit_cmp);
		for (int64 j = 1; j < p->nmhit; j++)
			if (p->mhit[j].key != p->mhit[k - 1].key)
				p->mhit[k++] = p->mhit[j];
		p->nmhit = k;
	}
	p->mut_ndocs = ndocs;
	p->mut_sum_dl = sum_dl;
	bind_stats(p, ndocs, sum_dl);
}

/* ---------- parallel bind ---------- */

typedef struct SharedHit
{
	SiftKey		key;
	float4		score;
} SharedHit;

/* What follows the header: nmhit SharedHit, then for every leaf a count and that many MutTermEnt */
typedef struct SharedBind
{
	SiftDirSnapshot dir;
	uint64		mut_ndocs;
	uint64		mut_sum_dl;
	int64		nmhit;
	int			nleaves;
} SharedBind;

void *
sift_plan_export(const SiftPlan *p, Size *len)
{
	StringInfoData buf;
	SharedBind	h = {.dir = p->st->dir, .mut_ndocs = p->mut_ndocs, .mut_sum_dl = p->mut_sum_dl,
					 .nmhit = p->nmhit, .nleaves = p->nleaves};

	initStringInfo(&buf);
	appendBinaryStringInfo(&buf, &h, sizeof(h));
	for (int64 i = 0; i < p->nmhit; i++)
	{
		SharedHit	sh = {p->mhit[i].key, p->mhit[i].score};

		appendBinaryStringInfo(&buf, &sh, sizeof(sh));
	}
	for (int i = 0; i < p->nleaves; i++)
	{
		const EvalLeaf *l = &p->leaves[i];
		int32		n = l->mtab ? hash_get_num_entries(l->mtab) : 0;
		HASH_SEQ_STATUS seq;
		MutTermEnt *e;

		appendBinaryStringInfo(&buf, &n, sizeof(n));
		if (n > 0)
		{
			hash_seq_init(&seq, l->mtab);
			while ((e = hash_seq_search(&seq)) != NULL)
				appendBinaryStringInfo(&buf, e, sizeof(MutTermEnt));
		}
	}
	*len = buf.len;
	return buf.data;
}

const SiftDirSnapshot *
sift_shared_dir(const void *shared)
{
	return &((const SharedBind *) shared)->dir;
}

/* The mutable half of the bind as the exporting participant had it */
static void
import_mutable(SiftPlan *p, const SharedBind *h)
{
	const char *at = (const char *) (h + 1);

	if (h->nleaves != p->nleaves)
		elog(ERROR, "sift: parallel participants bound different queries");
	p->mut_ndocs = h->mut_ndocs;
	p->mut_sum_dl = h->mut_sum_dl;
	p->nmhit = h->nmhit;
	p->mhit = MemoryContextAllocHuge(p->mcxt, Max(h->nmhit, 1) * sizeof(MutHit));
	for (int64 i = 0; i < h->nmhit; i++, at += sizeof(SharedHit))
	{
		SharedHit	sh;

		memcpy(&sh, at, sizeof(sh));
		p->mhit[i] = (MutHit) {sh.key, sh.score, NULL};
	}
	for (int i = 0; i < p->nleaves; i++)
	{
		int32		n;

		memcpy(&n, at, sizeof(n));
		at += sizeof(n);
		for (int32 j = 0; j < n; j++, at += sizeof(MutTermEnt))
		{
			MutTermEnt	e;

			memcpy(&e, at, sizeof(e));
			leaf_mut_term(p, &p->leaves[i], e.term)->df = e.df;
		}
	}
	bind_stats(p, h->mut_ndocs, h->mut_sum_dl);
}

/* The terms of a scoring leaf that only mutable docs have weigh like the others: by df over the whole index */
static int
leaf_mut_terms(EvalLeaf *l, LeafTerm **out)
{
	HASH_SEQ_STATUS seq;
	MutTermEnt *e;
	int			n = 0;

	if (l->mtab == NULL)
		return 0;
	hash_seq_init(&seq, l->mtab);
	while ((e = hash_seq_search(&seq)) != NULL)
	{
		LeafTerm   *t = palloc0(sizeof(LeafTerm));

		t->term = e->term;
		t->len = strlen(e->term);
		t->info.df = e->df;
		out[n++] = t;
	}
	return n;
}

/* The same term over all segments weighs the same: idf of its df over the index */
static void
weigh_leaf(SiftPlan *p, EvalLeaf *l)
{
	LeafTerm  **all;
	int			n = 0;

	for (int s = 0; s < p->nsegs; s++)
		n += l->nterms[s];
	all = MemoryContextAlloc(p->mcxt, Max(n + (l->mtab ? hash_get_num_entries(l->mtab) : 0), 1) * sizeof(LeafTerm *));
	n = 0;
	for (int s = 0; s < p->nsegs; s++)
		for (int i = 0; i < l->nterms[s]; i++)
			all[n++] = &l->terms[s][i];
	n += leaf_mut_terms(l, all + n);
	qsort(all, n, sizeof(LeafTerm *), leaf_term_cmp);
	l->uniq = all;
	for (int i = 0, j; i < n; i = j)
	{
		double		df = 0;
		float4		w;

		for (j = i; j < n && leaf_term_cmp(&all[i], &all[j]) == 0; j++)
			df += all[j]->info.df;
		w = sift_kern_term_weight(l->node->boost, sift_kern_idf(p->stats.ndocs, df), p->bm.k1);
		for (int k = i; k < j; k++)
			all[k]->w = w;
		all[l->nuniq++] = all[i];
	}
}

static void
bind_scoring(SiftPlan *p)
{
	const SiftOptions *o = p->st->opts;

	sift_kern_bm25_init(&p->bm, (float4) o->k1, (float4) o->b, p->stats.avgdl > 0 ? (float4) p->stats.avgdl : 1.0f);
	for (int i = 0; i < p->nleaves; i++)
		if (p->leaves[i].scoring)
			weigh_leaf(p, &p->leaves[i]);
}

/* The weighed term of the leaf with these bytes */
static const LeafTerm *
leaf_weighed(const EvalLeaf *l, const char *term, int len)
{
	LeafTerm	key = {.term = (char *) term,.len = len},
			   *k = &key;

	return *(LeafTerm **) bsearch(&k, l->uniq, l->nuniq, sizeof(LeafTerm *), leaf_term_cmp);
}

/* Best term of the leaf in a mutable doc, weighed like the others; a wide leaf has one term, with the tf of all its words */
static float4
mut_leaf_score(const EvalLeaf *l, const SiftMutDoc *d, float4 knorm)
{
	const char *t = d->terms;
	float4		best = 0;
	int			wtf = 0;

	for (int j = 0; j < d->nterms; t += d->len[j++])
		if (leaf_term_hit(l, t, d->len[j], d->hash[j]))
		{
			if (l->wide)
				wtf += d->tf[j];
			else
				best = Max(best, sift_kern_bm25(leaf_weighed(l, t, d->len[j])->w, d->tf[j], knorm));
		}
	return wtf > 0 ? sift_kern_bm25(leaf_weighed(l, l->wide, l->widelen)->w, Min(wtf, SIFT_MAX_TF), knorm) : best;
}

/*
 * Mutable matches score at bind from their records: the leaves in canonical order, a phrase as the AND
 * of its terms (the text decides at verify, like for the immutable candidates).
 */
static void
score_mutable(SiftPlan *p)
{
	for (int64 i = 0; i < p->nmhit; i++)
	{
		MutHit	   *h = &p->mhit[i];
		float4		knorm = sift_kern_knorm1(&p->bm, h->doc->dl),
					score = 0;

		for (int j = 0; j < p->nleaves; j++)
			if (p->leaves[j].scoring)
				score += mut_leaf_score(&p->leaves[j], h->doc, knorm);
		h->score = score;
	}
}

/* ---------- phrase positions ---------- */

/*
 * Decides the required phrases of a doc from the positions of their terms. The caller points the
 * context at the terms' groups of the doc's SB (posctx_group) and names the doc (phrases_hold).
 */
typedef struct PosCtx
{
	const SiftPlan *plan;
	MemoryContext mcxt;
	SiftPostingCursor **pc;		/* [leaf] the term's cursor and its group in the SB */
	const sift_grp **g;
	int		   *wpage;			/* [leaf] the walk over the group's pages: the term has widx pages below wpage, */
	uint32	   *widx;
	uint32	   *wrank;			/* ... holding wrank postings */
	SiftPosBuf *pb;				/* [leaf] positions in the current doc */
	uint64	   *stamp;			/* [leaf] the doc pb belongs to */
	uint64		doc;
	int			page;			/* the current doc: page of the SB and offset - 1 */
	int			off;
	uint32	   *start;			/* scratch: the phrase starts that fit the terms so far */
	uint32		startcap;
} PosCtx;

static PosCtx *
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

static void
posctx_group(PosCtx *c, int leaf, SiftPostingCursor *pc, const sift_grp *g)
{
	c->pc[leaf] = pc;
	c->g[leaf] = g;
	c->wpage[leaf] = 0;
	c->widx[leaf] = 0;
	c->wrank[leaf] = 0;
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

/* Does the current doc hold the phrase: the starts that fit the terms read so far, narrowed term by term, rarest first */
static bool
phrase_match(PosCtx *c, const PhraseCheck *ph)
{
	uint32		n = 0;

	for (int k = 0; k < ph->nterms; k++)
	{
		uint32		i = ph->order[k],
					j = 0,
					m = 0;
		const SiftPosBuf *pb = posctx_positions(c, ph->leaf[i]);

		if (k == 0)
		{
			if (pb->n > c->startcap)
			{
				c->startcap = Max(pb->n, 2 * c->startcap);
				c->start = c->start ? repalloc_huge(c->start, (Size) c->startcap * sizeof(uint32)) :
					MemoryContextAllocHuge(c->mcxt, (Size) c->startcap * sizeof(uint32));
			}
			for (; j < pb->n; j++)
				if (pb->v[j] >= i)
					c->start[m++] = pb->v[j] - i;
		}
		else
			for (uint32 x = 0; x < n; x++)
			{
				uint32		want = c->start[x] + i;

				while (j < pb->n && pb->v[j] < want)
					j++;
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
static bool
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
	p->mutno = MemoryContextAlloc(st->mcxt, Max(st->dir.nsegs, 1) * sizeof(int));
	for (int i = 0; i < st->dir.nsegs; i++)
	{
		if (st->dir.segs[i].kind != SIFT_SEG_IMMUTABLE)
		{
			p->mutno[p->nmut++] = i;
			continue;
		}
		p->segno[p->nsegs++] = i;
		p->stats.ndocs += st->dir.segs[i].ndocs;
		p->stats.avgdl += st->dir.segs[i].sum_dl;
	}
	p->stats.nmutable = p->nmut;
	p->capped = (flags & SIFT_BIND_CAPPED) != 0;
	p->scoring = (flags & SIFT_BIND_SCORE) != 0;
	p->seg = MemoryContextAlloc(st->mcxt, Max(p->nsegs, 1) * sizeof(SiftSegment *));
	for (int s = 0; s < p->nsegs; s++)
		p->seg[s] = state_segment(st, p->segno[s]);

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
		import_mutable(p, shared);
	else
		bind_mutable(p);
	phrase_prepare(p);
	if (p->scoring)
	{
		bind_scoring(p);
		if (!shared)
			score_mutable(p);
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

/* ---------- iterator ---------- */

/*
 * What the iterator keeps of a segment's tree: the leaves' cursors, from which the phrase walkers
 * read the groups of the matches, and the context that decides phrases
 */
typedef struct IterCurs
{
	SiftPostingCursor ***leafpc;	/* [leaf] -> the cursors of the leaf's terms in this segment */
	PosCtx	   *pos;
} IterCurs;

struct SiftMatchIter
{
	SiftPlan   *plan;
	sift_cur  **root;			/* per segment, NULL: no matches */
	sift_cur  **sure;			/* per segment: the matches that need no recheck, NULL: none (plan->sure) */
	const sift_grp **grp;		/* scratch: current group per segment */
	int64		mpos;			/* next mutable match */
	uint32		next_sb;
	uint32		sb_end;			/* the visited range ends before this superblock */
	SiftNextRangeFn next_range;	/* parallel: where the next range comes from */
	void	   *range_arg;
	uint32		sb;
	int			page;			/* next page of the loaded SB to return */
	uint64		pages[SIFT_PAGEMASK_WORDS];
	uint64	  (*om)[SIFT_OFFMASK_WORDS];	/* merged offset masks by page of the SB */
	PosCtx	   *pos;
	IterCurs   *curs;				/* [segment] */
	uint64		heap[SIFT_PAGEMASK_WORDS];	/* pages of the SB whose candidates the heap must recheck */
	uint64		spages[SIFT_PAGEMASK_WORDS];	/* the same for the matches that need no recheck */
	uint64	  (*som)[SIFT_OFFMASK_WORDS];
};

static uint8 *
cursor_buf(MemoryContext cx)
{
	return MemoryContextAllocAligned(cx, SIFT_CUR_BUF, SIFT_SIMD_ALIGN, 0);
}

/*
 * The cursors of one segment's tree. The ranker repositions them (posting
 * cursors rewind, operator cursors reset) and scores from the posting
 * cursors of the leaves; the iterator needs neither (cs NULL).
 */
typedef struct SegCurs
{
	List	   *posting;			/* SiftPostingCursor * */
	List	   *ops;				/* sift_cur * */
	SiftPostingCursor ***leafpc;	/* [leaf] -> the cursors of the leaf's terms in this segment */
	sift_cur  **leafor;				/* [leaf] -> its OR, for the multi-term scoring leaves the ranker narrows per unit */
} SegCurs;

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

	for (int i = 0; i < ph->nterms; i++)
	{
		SiftPostingCursor *tc = pc->ic->leafpc[ph->leaf[i]][0];
		const sift_cur *cur = sift_post_cursor(tc);

		if (cur->state != 1 || cur->g.gid != g->gid)
			elog(ERROR, "sift: a phrase term is not positioned at superblock %u", g->gid);
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
		if (phrase_group(pc, g))
		{
			c->state = 1;
			return &c->g;
		}
	c->state = 2;
	return NULL;
}

/* kid filtered by phrase ph when the segment's positions decide it; kid itself otherwise */
static sift_cur *
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

/* AND / OR of k >= 1 cursors; the single cursor itself for k = 1 */
static sift_cur *
op_cursor(SegCurs *cs, MemoryContext cx, EvalKind kind, sift_cur **kid, int k)
{
	sift_cur   *c;

	if (k == 1)
		return kid[0];
	c = MemoryContextAllocZero(cx, sizeof(sift_cur));
	if (kind == EN_AND)
		sift_cur_and(c, kid, k, cursor_buf(cx));
	else
		sift_cur_or(c, kid, k, cursor_buf(cx));
	if (cs)
		cs->ops = list_append_unique_ptr(cs->ops, c);
	return c;
}

static sift_cur *
leaf_cursor(SiftPlan *p, int s, const EvalLeaf *l, SegCurs *cs, const IterCurs *ic, MemoryContext cx)
{
	sift_cur  **kid = MemoryContextAlloc(cx, Max(l->nterms[s], 1) * sizeof(sift_cur *));
	SiftPostingCursor **pc = MemoryContextAlloc(cx, Max(l->nterms[s], 1) * sizeof(SiftPostingCursor *));
	bool		narrowed = cs && l->scoring && l->nterms[s] > 1;
	sift_cur   *orc = NULL;

	for (int i = 0; i < l->nterms[s]; i++)
	{
		const LeafTerm *t = &l->terms[s][i];

		pc[i] = l->wide ? sift_post_open_blob(&t->info, t->blob, cx) : sift_post_open(&p->seg[s]->sr, &t->info, cx);
		kid[i] = sift_post_cursor(pc[i]);
		if (cs && !narrowed)
			cs->posting = lappend(cs->posting, pc[i]);
	}
	if (l->nterms[s] > 0)
		orc = op_cursor(cs, cx, EN_OR, kid, l->nterms[s]);
	if (cs)
		cs->leafpc[l - p->leaves] = pc;
	else if (ic)
		ic->leafpc[l - p->leaves] = pc;
	if (narrowed)
		cs->leafor[l - p->leaves] = orc;
	return orc;
}

/* Cursor over the segment's matches of n, NULL when there are none; without cs, ic keeps every leaf's cursors */
static sift_cur *
seg_cursor(SiftPlan *p, int s, const EvalNode *n, SegCurs *cs, const IterCurs *ic, MemoryContext cx)
{
	sift_cur  **pos,
			  **neg,
			   *c;
	int			npos = 0,
				nneg = 0;
	bool		dead = false;

	if (n->kind == EN_NOTHING)
		return NULL;
	if (n->kind == EN_LEAF)
		return n->phrase < 0 ? leaf_cursor(p, s, &p->leaves[n->leaf], cs, ic, cx) :
			phrase_cursor(p, s, n->phrase, leaf_cursor(p, s, &p->leaves[n->leaf], cs, ic, cx), ic, cx);
	pos = MemoryContextAlloc(cx, n->nchild * sizeof(sift_cur *));
	neg = MemoryContextAlloc(cx, n->nchild * sizeof(sift_cur *));
	for (int i = 0; i < n->nchild; i++)
	{
		const EvalNode *k = n->child[i];
		bool		isnot = k->kind == EN_NOT;

		c = seg_cursor(p, s, isnot ? k->child[0] : k, cs, ic, cx);
		if (c == NULL && n->kind == EN_AND && !isnot)
		{
			/* the ranker needs every leaf's cursors, even those of a branch that cannot match */
			if (cs == NULL)
				return NULL;
			dead = true;
		}
		if (c != NULL)
			(isnot ? neg : pos)[isnot ? nneg++ : npos++] = c;
	}
	if (npos == 0 || dead)
		return NULL;
	c = op_cursor(cs, cx, n->kind, pos, npos);
	if (n->phrase >= 0)
		c = phrase_cursor(p, s, n->phrase, c, ic, cx);
	if (nneg > 0)
	{
		sift_cur  **kid = MemoryContextAlloc(cx, 2 * sizeof(sift_cur *));
		sift_cur   *an = MemoryContextAllocZero(cx, sizeof(sift_cur));

		kid[0] = c;
		kid[1] = op_cursor(cs, cx, EN_OR, neg, nneg);
		sift_cur_andnot(an, kid, cursor_buf(cx));
		if (cs)
			cs->ops = lappend(cs->ops, an);
		c = an;
	}
	return c;
}

SiftMatchIter *
sift_iter_begin(SiftPlan *plan)
{
	SiftMatchIter *it = MemoryContextAllocZero(plan->mcxt, sizeof(SiftMatchIter));

	it->plan = plan;
	it->root = MemoryContextAllocZero(plan->mcxt, Max(plan->nsegs, 1) * sizeof(sift_cur *));
	it->grp = MemoryContextAllocZero(plan->mcxt, Max(plan->nsegs, 1) * sizeof(sift_grp *));
	it->om = MemoryContextAllocAligned(plan->mcxt, SIFT_SB_PAGES * SIFT_OFFMASK_WORDS * sizeof(uint64),
									   SIFT_SIMD_ALIGN, 0);
	it->page = SIFT_SB_PAGES;
	it->sb_end = UINT32_MAX;
	if (plan->nphr > 0)
	{
		it->pos = posctx_create(plan);
		it->curs = MemoryContextAllocZero(plan->mcxt, Max(plan->nsegs, 1) * sizeof(IterCurs));
	}
	if (plan->heap)
	{
		it->som = MemoryContextAllocAligned(plan->mcxt, SIFT_SB_PAGES * SIFT_OFFMASK_WORDS * sizeof(uint64),
											SIFT_SIMD_ALIGN, 0);
		it->sure = MemoryContextAllocZero(plan->mcxt, Max(plan->nsegs, 1) * sizeof(sift_cur *));
	}
	for (int s = 0; s < plan->nsegs; s++)
	{
		if (it->curs)
		{
			it->curs[s].leafpc = MemoryContextAllocZero(plan->mcxt, Max(plan->nleaves, 1) * sizeof(SiftPostingCursor **));
			it->curs[s].pos = it->pos;
		}
		it->root[s] = seg_cursor(plan, s, plan->root, NULL, it->curs ? &it->curs[s] : NULL, plan->mcxt);
		if (it->sure && plan->sure)
			it->sure[s] = seg_cursor(plan, s, plan->sure, NULL, NULL, plan->mcxt);
		sift_sr_release(&plan->seg[s]->sr);
	}
	return it;
}

void
sift_iter_ranges(SiftMatchIter *it, SiftNextRangeFn next, void *arg)
{
	it->next_range = next;
	it->range_arg = arg;
	it->sb_end = it->next_sb;
}

void
sift_iter_end(SiftMatchIter *it)
{
	for (int s = 0; s < it->plan->nsegs; s++)
		sift_sr_release(&it->plan->seg[s]->sr);
}

/* Points the phrase walkers at the groups the tree's cursors hold in SB gid: every term of a match is there */
static void
iter_phrase_groups(SiftMatchIter *it, int s, uint32 gid)
{
	const SiftPlan *p = it->plan;

	for (int k = 0; k < p->npleaf; k++)
	{
		SiftPostingCursor *pc = it->curs[s].leafpc[p->pleaf[k]][0];
		const sift_cur *c = sift_post_cursor(pc);

		if (c->state != 1 || c->g.gid != gid)
			elog(ERROR, "sift: a phrase term is not positioned at superblock %u", gid);
		posctx_group(it->pos, p->pleaf[k], pc, &c->g);
	}
}

/* Clears the docs of mask that lack a phrase the positions decide */
static void
phrases_filter(PosCtx *c, int s, int page, uint64 *mask)
{
	for (int k = 0; k < SIFT_OFFMASK_WORDS; k++)
		for (uint64 m = mask[k]; m; m &= m - 1)
			if (!phrases_hold(c, s, page, k * 64 + pg_rightmost_one_pos64(m)))
				mask[k] &= ~(m & -m);
}

/* ORs the offsets of mask into a page of an SB buffer (pages, om) */
static void
mask_or(uint64 *pages, uint64 (*om)[SIFT_OFFMASK_WORDS], int page, const uint64 *mask)
{
	if (!(pages[page / 64] & (UINT64CONST(1) << (page & 63))))
	{
		memset(om[page], 0, SIFT_OFFMASK_WORDS * sizeof(uint64));
		pages[page / 64] |= UINT64CONST(1) << (page & 63);
	}
	for (int k = 0; k < SIFT_OFFMASK_WORDS; k++)
		om[page][k] |= mask[k];
}

/*
 * ORs the live part of one segment's group into the SB buffer, minus the docs the positions refute. The sure
 * group (the sure tree's) goes to the buffer of the matches that need no recheck instead. So do the matches
 * of a segment whose positions decide every phrase, unless a negated phrase is left to the heap.
 */
static void
iter_merge(SiftMatchIter *it, int s, const sift_grp *g, bool sure)
{
	const SiftPlan *p = it->plan;
	SiftSegment *seg = p->seg[s];
	const uint8 *off = g->off;
	SiftDocChunk dc;
	bool		dirty = (seg->entry.flags & SIFT_SEG_DIRTY) && sift_seg_docs(seg, g->gid, &dc) && dc.dirty;
	bool		pos = !sure && p->npleaf > 0,
				decided = !sure && it->som && !p->hardrecheck && !p->segheap[s];

	if (pos)
		iter_phrase_groups(it, s, g->gid);

	for (int wd = 0; wd < SIFT_PAGEMASK_WORDS; wd++)
		for (uint64 m = g->pages[wd]; m; m &= m - 1, off += g->w)
		{
			int			page = wd * 64 + pg_rightmost_one_pos64(m);
			uint64		mask[SIFT_OFFMASK_WORDS] = {0};
			bool		any = false;

			memcpy(mask, off, g->w);
			if (dirty)
			{
				uint64		live[SIFT_OFFMASK_WORDS];

				sift_seg_page_live(seg, &dc, page, live);
				for (int k = 0; k < SIFT_OFFMASK_WORDS; k++)
					mask[k] &= live[k];
			}
			if (pos)
				phrases_filter(it->pos, s, page, mask);
			for (int k = 0; k < SIFT_OFFMASK_WORDS; k++)
				any |= mask[k] != 0;
			if (!any)
				continue;
			if (sure)
			{
				mask_or(it->spages, it->som, page, mask);
				continue;
			}
			if (p->nphr > 0 && p->segheap[s])
				it->heap[wd] |= UINT64CONST(1) << (page & 63);
			mask_or(it->pages, it->om, page, mask);
			if (decided)
				mask_or(it->spages, it->som, page, mask);
		}
	sift_sr_release(&seg->sr);
}

/* Sets the mutable matches of SB sb in the SB buffer */
static void
iter_merge_mutable(SiftMatchIter *it, uint32 sb)
{
	const SiftPlan *p = it->plan;

	for (; it->mpos < p->nmhit && sift_block_sb(sift_key_block(p->mhit[it->mpos].key)) == sb; it->mpos++)
	{
		SiftKey		key = p->mhit[it->mpos].key;
		int			page = sift_key_block(key) & (SIFT_SB_PAGES - 1),
					off = (key & 0xFFFF) - 1;

		if (!(it->pages[page / 64] & (UINT64CONST(1) << (page & 63))))
		{
			memset(it->om[page], 0, SIFT_OFFMASK_WORDS * sizeof(uint64));
			it->pages[page / 64] |= UINT64CONST(1) << (page & 63);
		}
		it->om[page][off / 64] |= UINT64CONST(1) << (off & 63);
		if (p->nphr > 0)
			it->heap[page / 64] |= UINT64CONST(1) << (page & 63);
	}
}

/*
 * Seeks the segments' cursors to the next SB with matches in any of them or in the mutable segments
 * (it->grp[s] are their groups there); false at the end
 */
static bool
iter_find_sb(SiftMatchIter *it, uint32 *sbp)
{
	for (;;)
	{
		uint32		sb = UINT32_MAX;

		if (it->next_range && it->next_sb >= it->sb_end)
		{
			uint32		lo,
						hi;

			if (!it->next_range(it->range_arg, &lo, &hi))
				return false;
			it->next_sb = lo;
			it->sb_end = hi;
			while (it->mpos < it->plan->nmhit && sift_block_sb(sift_key_block(it->plan->mhit[it->mpos].key)) < lo)
				it->mpos++;
		}
		for (int s = 0; s < it->plan->nsegs; s++)
		{
			it->grp[s] = it->root[s] ? sift_seek(it->root[s], it->next_sb) : NULL;
			if (it->grp[s] && it->grp[s]->gid < sb)
				sb = it->grp[s]->gid;
		}
		if (it->mpos < it->plan->nmhit)
			sb = Min(sb, sift_block_sb(sift_key_block(it->plan->mhit[it->mpos].key)));
		if (sb >= it->sb_end)
		{
			if (!it->next_range)
				return false;
			it->next_sb = it->sb_end;
			continue;
		}
		CHECK_FOR_INTERRUPTS();
		*sbp = sb;
		return true;
	}
}

/* Loads the next SB with matches in any segment; false at the end */
static bool
iter_load_sb(SiftMatchIter *it)
{
	uint32		sb;

	while (iter_find_sb(it, &sb))
	{
		memset(it->pages, 0, sizeof(it->pages));
		memset(it->heap, 0, sizeof(it->heap));
		memset(it->spages, 0, sizeof(it->spages));
		for (int s = 0; s < it->plan->nsegs; s++)
			if (it->grp[s] && it->grp[s]->gid == sb)
			{
				const sift_grp *sg = it->sure && it->sure[s] ? sift_seek_eq(it->sure[s], sb) : NULL;

				iter_merge(it, s, it->grp[s], false);
				if (sg)
					iter_merge(it, s, sg, true);
			}
		iter_merge_mutable(it, sb);
		it->next_sb = sb + 1;
		it->plan->stats.sb_scanned++;
		if (it->pages[0] | it->pages[1] | it->pages[2] | it->pages[3])
		{
			it->sb = sb;
			it->page = 0;
			return true;
		}
	}
	return false;
}

/* Next set page at or after it->page; SIFT_SB_PAGES when none */
static int
iter_next_page(const SiftMatchIter *it)
{
	for (int wd = it->page / 64; wd < SIFT_PAGEMASK_WORDS; wd++)
	{
		uint64		m = it->pages[wd] & (wd == it->page / 64 ? ~UINT64CONST(0) << (it->page & 63) : ~UINT64CONST(0));

		if (m)
			return wd * 64 + pg_rightmost_one_pos64(m);
	}
	return SIFT_SB_PAGES;
}

bool
sift_iter_next(SiftMatchIter *it, SiftPageMatch *out)
{
	int			page = iter_next_page(it);

	if (page == SIFT_SB_PAGES)
	{
		if (!iter_load_sb(it))
			return false;
		page = iter_next_page(it);
	}
	out->blk = (BlockNumber) it->sb << SIFT_SB_SHIFT | page;
	memcpy(out->offmask, it->om[page], sizeof(out->offmask));
	memset(out->sure, 0, sizeof(out->sure));
	out->recheck = it->plan->hardrecheck || (it->heap[page / 64] >> (page & 63) & 1);
	if (out->recheck && it->spages[page / 64] >> (page & 63) & 1)
	{
		uint64		undecided = 0;

		memcpy(out->sure, it->som[page], sizeof(out->sure));
		for (int k = 0; k < SIFT_OFFMASK_WORDS; k++)
			undecided |= out->offmask[k] & ~out->sure[k];
		out->recheck = undecided != 0;
	}
	it->page = page + 1;
	return true;
}

/* ---------- ranker ---------- */

/*
 * Best-first search over (segment, SB) units ordered by their score upper
 * bound, then over the 16-page scoring blocks of a unit. A unit or block is
 * dropped once its bound is not above theta (the worst kept score, so equal
 * scores still compete on ctid). Candidates are scored exactly, verified by
 * the caller and only then enter the top-k; the sums follow the leaf order of
 * the bound query, as the reference does.
 */

#define BLK_SLOTS	(SIFT_BLK_PAGES * SIFT_KERN_SLOTS)

typedef uint64 BlockMask[SIFT_BLK_PAGES][SIFT_OFFMASK_WORDS];

typedef struct GidUB
{
	uint32		gid;
	float4		ub[SIFT_BLKS_PER_SB];
} GidUB;

/* Bounds of a leaf's postings per SB, ascending gid; pos walks them while the units are built */
typedef struct LeafUB
{
	int			n;
	int			pos;
	GidUB	   *list;
} LeafUB;

/* One dictionary term of a leaf in a segment and its group in the unit's SB */
typedef struct RkTerm
{
	SiftPostingCursor *pc;			/* the tree's cursor: bounds, clone source */
	SiftPostingCursor *sc;			/* own cursor for scoring: the tree's seeks skip groups */
	SiftPostingCursor *gc;			/* the cursor g belongs to: pc when the tree already decoded it, else sc */
	float4		w;
	const SiftGroupBounds *gb;		/* bounds per SB, ascending gid */
	int			ngb;
	float4		ubw;				/* bound weight: ub = ubw * blk_bound */
	float4		tub;				/* the largest ub of the term in the segment */
	const SiftGroupBounds *bb;		/* bounds in the unit's SB */
	bool		live;				/* may lift a document over theta somewhere in the unit */
	bool		act;				/* scores in the current block */
	uint32		pce;				/* RankSeg.epoch the cursors are positioned in */
	uint32		sce;
	const sift_grp *g;				/* NULL: no postings in this SB */
	uint8	   *tf;
	uint32		tfcap;
	bool		decoded;
	uint32		base[SIFT_BLKS_PER_SB + 1];	/* tf rank of each block's first posting */
} RkTerm;

typedef struct RankSeg
{
	int			s;
	SegCurs		cs;
	sift_cur   *root;
	int64		last;				/* last SB requested from the cursors */
	uint32		epoch;				/* bumped when the cursors go back to the start */
	LeafUB	   *lub;				/* [leaf] */
	float4	   *lmax;				/* [leaf] the largest ub of its terms */
	RkTerm	   *term;
	int			nterm;
	int		   *first;				/* [leaf] index of its first term */
} RankSeg;

typedef struct Unit
{
	int			seg;
	uint32		gid;
	float4		maxub;
	int			nblk;
	float4		ub[SIFT_BLKS_PER_SB];
} Unit;

typedef struct RankWork
{
	float4		scores[BLK_SLOTS] pg_attribute_aligned(SIFT_SIMD_ALIGN);
	float4		knorm[BLK_SLOTS] pg_attribute_aligned(SIFT_SIMD_ALIGN);
	float4		leafbuf[BLK_SLOTS] pg_attribute_aligned(SIFT_SIMD_ALIGN);
	float4		tmp[BLK_SLOTS] pg_attribute_aligned(SIFT_SIMD_ALIGN);
	float4		vals[BLK_SLOTS] pg_attribute_aligned(SIFT_SIMD_ALIGN);
	float4		part[BLK_SLOTS] pg_attribute_aligned(SIFT_SIMD_ALIGN);
	uint16		slot[BLK_SLOTS];
	BlockMask	res pg_attribute_aligned(SIFT_SIMD_ALIGN);
	BlockMask	tm pg_attribute_aligned(SIFT_SIMD_ALIGN);
	BlockMask	tmc pg_attribute_aligned(SIFT_SIMD_ALIGN);
	BlockMask	lu pg_attribute_aligned(SIFT_SIMD_ALIGN);
	BlockMask	flt pg_attribute_aligned(SIFT_SIMD_ALIGN);
	uint16		zero[SIFT_KERN_SLOTS];
	const uint16 *dl[SIFT_BLK_PAGES];
} RankWork;

struct SiftRanker
{
	SiftPlan   *plan;
	SiftVerifyFn verify;
	void	   *arg;
	MemoryContext tmpcx;
	RankSeg    *seg;
	Unit	   *unit;
	int			nunit;
	RankWork   *wk;
	float4	   *leafub;				/* [leaf] scratch: its ub in the current block */
	int		   *ord;				/* [leaf] scratch: the leaves present in the block, best bound first */
	float4	   *rest;				/* [leaf] scratch: the bounds of the leaves after ord[i] */
	int64		k;
	int64		K;					/* hits kept by the current search */
	SiftHit    *hit;				/* keyed by the visible tuple's ctid */
	HTAB	   *hot;				/* visible ctid -> index entry, where they differ */
	int			nhit;
	int64		pos;
	bool		searched;
	bool		complete;
	SiftPar    *par;				/* parallel: the first search is shared with the other participants */
	bool		par_active;			/* ... and is running now */
	PosCtx	   *phrpos;
	bool		posok;				/* the phrase walkers sit on the current unit's groups */
};

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
		float4		w = sift_kern_ub_weight(l->terms[rs->s][i].w, p->stats.avgdl, ref);

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
		t->ubw = sift_kern_ub_weight(t->w, p->stats.avgdl, ref);
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
sift_rank_end(SiftRanker *r)
{
	hot_forget(r);
	for (int s = 0; s < r->plan->nsegs; s++)
		sift_sr_release(&r->plan->seg[s]->sr);
	MemoryContextDelete(r->tmpcx);
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
	int			n;

	if (!term_mask(g, b, wk->res))
		memset(wk->res, 0, sizeof(BlockMask));
	if (fs && !filter_result(p, fs, b, wk))
		return 0;
	for (int pg = 0; pg < SIFT_BLK_PAGES; pg++)
	{
		uint64		present[SIFT_OFFMASK_WORDS];
		uint64		live[SIFT_OFFMASK_WORDS];

		sift_seg_page(seg, dc, b * SIFT_BLK_PAGES + pg, present, &wk->dl[pg]);
		if (wk->dl[pg] == NULL)
			wk->dl[pg] = wk->zero;
		if (dc->dirty)
		{
			sift_seg_page_live(seg, dc, b * SIFT_BLK_PAGES + pg, live);
			for (int k = 0; k < SIFT_OFFMASK_WORDS; k++)
				wk->res[pg][k] &= live[k];
		}
		for (int k = 0; k < SIFT_OFFMASK_WORDS; k++)
			any |= wk->res[pg][k] != 0;
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

/* The exact score of a document from its text, for plans whose phrases are optional */
static float4
doc_score(SiftPlan *p, SiftDoc *d)
{
	float4		kn = sift_kern_knorm1(&p->bm, Min(sift_doc_ntokens(d), SIFT_MAX_DL)),
				score = 0;

	for (int i = 0; i < p->nleaves; i++)
	{
		const EvalLeaf *l = &p->leaves[i];
		float4		best = 0;

		if (!l->scoring || (l->phrase && !sift_doc_match(d, l->phrase)))
			continue;
		for (int j = 0; j < l->nuniq; j++)
		{
			int			tf = sift_doc_term_tf(d, l->uniq[j]->term, l->uniq[j]->len);

			if (tf > 0)
				best = Max(best, sift_kern_bm25(l->uniq[j]->w, Min(tf, SIFT_MAX_TF), kn));
		}
		score += best;
	}
	return score;
}

/* The exact recheck of a candidate's text; rescores in place when the index score is only a bound */
static bool
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
sift_plan_recheck(SiftPlan *plan, text *doc)
{
	return doc_pass(plan, doc, NULL);
}

bool
sift_plan_recheck_needed(const SiftPlan *plan)
{
	return plan->heap;
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
rank_mutable(SiftRanker *r, SiftTopK *tk)
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

	rank_mutable(r, tk);
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
	if (sift_par_claim_mutable(r->par))
		rank_mutable(r, tk);
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
	SiftParHit *mine = palloc(Max(r->nhit, 1) * sizeof(SiftParHit)),
			   *all;
	int64		n;

	for (int i = 0; i < r->nhit; i++)
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
	for (int i = 0; i < r->nhit; i++)
	{
		r->hit[i] = (SiftHit) {all[i].key, all[i].score};
		if (all[i].root != all[i].key)
			hot_remember(r, all[i].key, all[i].root);
	}
	rank_skip_units(r, sift_par_units_claimed(r->par, r->nunit), r->nunit);
	pfree(all);
}

/* Searches the K best verified hits from scratch */
static void
rank_search(SiftRanker *r)
{
	SiftPlan   *p = r->plan;
	SiftTopK	tk;
	bool		shared = r->par && !r->searched;

	hot_forget(r);
	if (r->hit)
		pfree(r->hit);
	r->hit = MemoryContextAlloc(p->mcxt, r->K * sizeof(SiftHit));
	sift_kern_topk_init(&tk, r->hit, r->K);
	p->stats.blocks_scored = p->stats.blocks_skipped = p->stats.candidates = p->stats.verified = 0;
	if (shared)
		rank_units_shared(r, &tk);
	else
		rank_units(r, &tk);
	p->stats.theta = sift_kern_topk_theta(&tk);
	r->complete = tk.n < r->K;
	r->nhit = sift_kern_topk_sort(&tk);
	r->searched = true;
	if (shared)
		rank_merge_shared(r);
}

#define RANK_INITIAL_K	1024

/*
 * A consumer that pulls past K rows is lossy (a join or a filter above the scan): every search scores
 * the blocks again, so the next K grows fast while the hit array is small (an unfiltered join passes
 * about a tenth of the rows, a search of 160 serves it where 40 did not) and slowly beyond it.
 */
#define RANK_FAST_GROWTH_K	4096

static int64
rank_next_k(int64 k)
{
	return Min(k * (k < RANK_FAST_GROWTH_K ? 16 : 4), INT_MAX / 2);
}

static void
rank_ensure(SiftRanker *r)
{
	if (!r->searched)
		r->K = r->k > 0 ? r->k : RANK_INITIAL_K;
	else if (r->pos >= r->nhit && !r->complete)
		r->K = rank_next_k(r->K);
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

/* ---------- count ---------- */

typedef struct CountCtx
{
	SiftPlan   *plan;
	SiftVis    *vis;
	SiftVerifyFn verify;
	void	   *arg;
	MemoryContext tmpcx;
	uint32		sb;				/* the superblock allvis belongs to */
	uint64		allvis[SIFT_PAGEMASK_WORDS];
} CountCtx;

/* The all-visible pages of superblock sb, read once per superblock */
static const uint64 *
allvisible(CountCtx *cc, uint32 sb)
{
	if (sb != cc->sb)
	{
		cc->sb = sb;
		sift_vis_sb_mask(cc->vis, sb, cc->allvis);
	}
	return cc->allvis;
}

/* Keeps the visible offsets of a page of candidates: the VM bit decides an all-visible page, else the heap */
static int
visible_offsets(CountCtx *cc, BlockNumber blk, uint64 *offmask)
{
	uint32		page = blk & (SIFT_SB_PAGES - 1);

	return allvisible(cc, sift_block_sb(blk))[page / 64] >> (page % 64) & 1 ? sift_offmask_popcount(offmask, 1) :
		sift_vis_filter_page(cc->vis, blk, offmask);
}

/*
 * Matches of a page with a phrase: the candidates the index does not decide are read and rechecked, the
 * others only checked for visibility; the offsets that pass stay in the mask
 */
static int
recheck_page(CountCtx *cc, SiftPageMatch *pm)
{
	uint64		sure[SIFT_OFFMASK_WORDS],
				any = 0;
	int			n = 0;

	memcpy(sure, pm->sure, sizeof(sure));
	for (int wd = 0; wd < SIFT_OFFMASK_WORDS; wd++)
	{
		any |= sure[wd];
		for (uint64 m = pm->offmask[wd] & ~sure[wd]; m; m &= m - 1)
		{
			ItemPointerData tid;
			text	   *doc = NULL;
			MemoryContext old = MemoryContextSwitchTo(cc->tmpcx);

			ItemPointerSet(&tid, pm->blk, wd * 64 + pg_rightmost_one_pos64(m) + 1);
			cc->plan->stats.candidates++;
			if (cc->verify(cc->arg, &tid, true, &doc) && doc_pass(cc->plan, doc, NULL))
				n++;
			else
				pm->offmask[wd] &= ~(m & -m);
			MemoryContextSwitchTo(old);
			MemoryContextReset(cc->tmpcx);
		}
	}
	if (any)
	{
		uint64		seen[SIFT_OFFMASK_WORDS];

		memcpy(seen, sure, sizeof(seen));
		n += visible_offsets(cc, pm->blk, sure);
		for (int wd = 0; wd < SIFT_OFFMASK_WORDS; wd++)
			pm->offmask[wd] &= ~(seen[wd] & ~sure[wd]);
	}
	return n;
}

/* The visible matches of the superblock ranges next hands out (next may be NULL: all), page by page */
static int64
eval_pages(SiftPlan *plan, SiftVis *vis, SiftVerifyFn verify, void *arg, SiftPageFn fn, void *fnarg,
		   SiftNextRangeFn next, void *next_arg)
{
	CountCtx	cc = {plan, vis, verify, arg, AllocSetContextCreate(CurrentMemoryContext, "sift count", ALLOCSET_DEFAULT_SIZES), UINT32_MAX};
	SiftMatchIter *it = sift_iter_begin(plan);
	SiftPageMatch pm;
	int64		n = 0;

	if (next)
		sift_iter_ranges(it, next, next_arg);

	while (sift_iter_next(it, &pm))
	{
		int			left;

		CHECK_FOR_INTERRUPTS();
		left = pm.recheck ? recheck_page(&cc, &pm) : visible_offsets(&cc, pm.blk, pm.offmask);
		if (left == 0)
			continue;
		n += left;
		fn(fnarg, pm.blk, pm.offmask);
	}
	sift_iter_end(it);
	MemoryContextDelete(cc.tmpcx);
	plan->stats.verified = n;
	return n;
}

int64
sift_eval_pages(SiftPlan *plan, SiftVis *vis, SiftVerifyFn verify, void *arg, SiftPageFn fn, void *fnarg)
{
	return eval_pages(plan, vis, verify, arg, fn, fnarg, NULL, NULL);
}

static void
count_page(void *arg, BlockNumber blk, const uint64 *offmask)
{
}

/*
 * The visible matches of one segment's group of a superblock, counted where they lie in the group: the
 * all-visible pages of a clean group in one pass, the others page by page
 */
static int64
count_group(CountCtx *cc, SiftSegment *seg, const sift_grp *g)
{
	SiftDocChunk dc;
	const uint8 *off = g->off;
	const uint64 *allvis = allvisible(cc, g->gid);
	uint64		bulk[SIFT_PAGEMASK_WORDS];
	int64		n;
	bool		dirty = (seg->entry.flags & SIFT_SEG_DIRTY) && sift_seg_docs(seg, g->gid, &dc) && dc.dirty;

	for (int wd = 0; wd < SIFT_PAGEMASK_WORDS; wd++)
		bulk[wd] = dirty ? 0 : g->pages[wd] & allvis[wd];
	n = sift_grp_count_mask(g, bulk);
	for (int wd = 0; wd < SIFT_PAGEMASK_WORDS; wd++)
		for (uint64 m = g->pages[wd]; m; m &= m - 1, off += g->w)
		{
			int			page = wd * 64 + pg_rightmost_one_pos64(m);
			uint64		mask[SIFT_OFFMASK_WORDS] = {0};

			if (bulk[wd] >> (page & 63) & 1)
				continue;
			memcpy(mask, off, g->w);
			if (dirty)
			{
				uint64		live[SIFT_OFFMASK_WORDS];

				sift_seg_page_live(seg, &dc, page, live);
				for (int k = 0; k < SIFT_OFFMASK_WORDS; k++)
					mask[k] &= live[k];
			}
			n += visible_offsets(cc, (BlockNumber) g->gid << SIFT_SB_SHIFT | page, mask);
		}
	sift_sr_release(&seg->sr);
	return n;
}

/* count(*) of a plan the bitmaps decide alone, without the mutable segments' matches: no page is copied */
static int64
count_visible(SiftPlan *plan, SiftVis *vis, SiftNextRangeFn next, void *next_arg)
{
	CountCtx	cc = {.plan = plan,.vis = vis,.sb = UINT32_MAX};
	SiftMatchIter *it = sift_iter_begin(plan);
	int64		n = 0;
	uint32		sb;

	if (next)
		sift_iter_ranges(it, next, next_arg);
	while (iter_find_sb(it, &sb))
	{
		for (int s = 0; s < plan->nsegs; s++)
			if (it->grp[s] && it->grp[s]->gid == sb)
				n += count_group(&cc, plan->seg[s], it->grp[s]);
		it->next_sb = sb + 1;
		plan->stats.sb_scanned++;
	}
	sift_iter_end(it);
	plan->stats.verified = n;
	return n;
}

int64
sift_eval_count_ranges(SiftPlan *plan, SiftVis *vis, SiftVerifyFn verify, void *arg,
					   SiftNextRangeFn next, void *next_arg)
{
	if (!plan->heap && plan->npleaf == 0 && plan->nmhit == 0)
		return count_visible(plan, vis, next, next_arg);
	return eval_pages(plan, vis, verify, arg, count_page, NULL, next, next_arg);
}

int64
sift_eval_count(SiftPlan *plan, SiftVis *vis, SiftVerifyFn verify, void *arg)
{
	return sift_eval_count_ranges(plan, vis, verify, arg, NULL, NULL);
}

/* ---------- estimate ---------- */

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
			none *= 1.0 - Min(1.0, l->terms[s][i].info.df / ndocs);
			est->postings += l->terms[s][i].info.df;
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

double
sift_plan_docs(const SiftPlan *p)
{
	Estimate	e = {0};

	return node_prob(p, p->root, &e) * p->stats.ndocs;
}

void
sift_eval_estimate(Relation index, text **queries, int nqueries, SiftEstimate *est)
{
	MemoryContext cx = AllocSetContextCreate(CurrentMemoryContext, "sift estimate", ALLOCSET_DEFAULT_SIZES);
	MemoryContext old = MemoryContextSwitchTo(cx);
	SiftIndexState *st = sift_state_open(index, cx);
	SiftQuery **q = palloc(nqueries * sizeof(SiftQuery *));
	SiftPlan   *p;
	Estimate	e = {0};

	for (int i = 0; i < nqueries; i++)
		q[i] = sift_state_query(st, VARDATA_ANY(queries[i]), VARSIZE_ANY_EXHDR(queries[i]));
	p = sift_plan_bind(st, q, nqueries, SIFT_BIND_CAPPED);
	est->docs = node_prob(p, p->root, &e) * p->stats.ndocs;
	est->postings = e.postings;
	est->terms = e.terms;
	est->ndocs = p->stats.ndocs;
	sift_state_close(st);
	MemoryContextSwitchTo(old);
	MemoryContextDelete(cx);
}
