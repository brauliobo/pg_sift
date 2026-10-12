/*
 * sift_eval.c - binds SiftQL queries to an index snapshot and evaluates them
 * over the segments' codec cursors (DESIGN.md (a), (h)).
 *
 * Bind relaxes each query to something the bitmaps can answer exactly or as
 * a superset (phrases), resolves every leaf to dictionary terms per segment
 * and keeps the term infos. The iterator builds a codec cursor tree per
 * segment (OR of a leaf's terms, AND of positives, ANDNOT of the negatives)
 * and merges the segments' superblock groups into heap-ordered pages.
 */
#include "postgres.h"

#include "common/int.h"
#include "miscadmin.h"
#include "port/pg_bitutils.h"
#include "utils/builtins.h"
#include "utils/memutils.h"
#include "utils/rel.h"

#include "sift_eval.h"
#include "sift_mutable.h"
#include "sift_segment.h"

typedef struct LeafTerm
{
	char	   *term;
	int			len;
	SiftTermInfo info;
} LeafTerm;

/* A query leaf and its dictionary terms in every segment */
typedef struct EvalLeaf
{
	const SiftQNode *node;
	SiftTermMatcher *m;			/* NULL for a plain term */
	uint32		hash;			/* plain term: sift_term_hash */
	int		   *nterms;			/* [nsegs] */
	LeafTerm  **terms;			/* [nsegs] */
} EvalLeaf;

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
} EvalNode;

struct SiftPlan
{
	SiftIndexState *st;
	MemoryContext mcxt;
	int			nsegs;
	int		   *segno;			/* directory index of each immutable segment */
	SiftSegment **seg;
	int			nmut;
	int		   *mutno;			/* directory index of each mutable segment */
	SiftKey    *mtids;			/* live matches of the mutable segments, ascending */
	int64		nmtids;
	int			nleaves;
	int			maxleaves;
	EvalLeaf   *leaves;
	EvalNode   *root;
	bool		recheck;
	bool		capped;			/* estimate: stop expanding at the limit */
	SiftPlanStats stats;
};

static EvalNode nothing = {EN_NOTHING};

/* ---------- state ---------- */

SiftIndexState *
sift_state_open(Relation index, MemoryContext mcxt)
{
	MemoryContext old = MemoryContextSwitchTo(mcxt);
	SiftIndexState *st = palloc0(sizeof(SiftIndexState));
	SiftAnalyzerConfig cfg;

	st->index = index;
	st->mcxt = mcxt;
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
	return n;
}

static EvalNode *
en_leaf(SiftPlan *p, const SiftQNode *node)
{
	EvalNode   *n = en_make(p, EN_LEAF, 0, NULL);

	if (p->nleaves == p->maxleaves)
	{
		p->maxleaves = Max(8, 2 * p->maxleaves);
		p->leaves = p->leaves ? repalloc(p->leaves, p->maxleaves * sizeof(EvalLeaf)) :
			MemoryContextAlloc(p->mcxt, p->maxleaves * sizeof(EvalLeaf));
	}
	p->leaves[p->nleaves] = (EvalLeaf) {node};
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

/* A phrase read as the AND of its distinct terms */
static EvalNode *
relax_phrase(SiftPlan *p, const SiftQNode *n)
{
	EvalNode  **kids = MemoryContextAlloc(p->mcxt, n->nterms * sizeof(EvalNode *));
	int			nk = 0;

	for (int i = 0; i < n->nterms; i++)
	{
		SiftQNode  *t;
		int			j;

		for (j = 0; j < i; j++)
			if (n->termlens[j] == n->termlens[i] && memcmp(n->terms[j], n->terms[i], n->termlens[i]) == 0)
				break;
		if (j < i)
			continue;
		t = MemoryContextAllocZero(p->mcxt, sizeof(SiftQNode));
		t->kind = SQ_TERM;
		t->text = n->terms[i];
		t->textlen = n->termlens[i];
		kids[nk++] = en_leaf(p, t);
	}
	return en_make(p, EN_AND, nk, kids);
}

/*
 * The tree the bitmaps evaluate: over = true yields a superset of the
 * matches (positive phrases become ANDs of their terms), false a subset (a
 * phrase matches nothing). NOT flips the direction, so what a NOT excludes
 * is never more than the exact exclusion.
 */
static EvalNode *
relax(SiftPlan *p, const SiftQNode *n, bool over)
{
	EvalNode  **kids;
	int			nk = 0;

	switch (n->kind)
	{
		case SQ_NOTHING:
			return &nothing;
		case SQ_PHRASE:
			return over ? relax_phrase(p, n) : &nothing;
		case SQ_AND:
		case SQ_OR:
			kids = MemoryContextAlloc(p->mcxt, n->nchild * sizeof(EvalNode *));
			for (int i = 0; i < n->nchild; i++)
			{
				const SiftQNode *c = n->child[i];
				bool		neg = c->kind == SQ_NOT;
				EvalNode   *e = relax(p, neg ? c->child[0] : c, neg ? !over : over);

				if (e->kind == EN_NOTHING && !neg && n->kind == SQ_AND)
					return &nothing;
				if (e->kind != EN_NOTHING)
					kids[nk++] = neg ? en_not(p, e) : e;
			}
			return nk == 0 ? &nothing : en_make(p, n->kind == SQ_AND ? EN_AND : EN_OR, nk, kids);
		default:
			return en_leaf(p, n);
	}
}

typedef struct Expand
{
	SiftPlan   *plan;
	const SiftTermMatcher *matcher;
	LeafTerm   *terms;
	int			n;
	int			cap;
	bool		over;
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
}

static bool
expand_visit(void *arg, const char *term, int len, const SiftTermInfo *info)
{
	Expand	   *e = arg;

	if (!sift_matcher_match(e->matcher, term, len))
		return true;
	if (e->n == sift_max_expansions)
	{
		e->over = true;
		return false;
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

/* Terms across segments are distinct only once: count them before binding */
static void
check_distinct(SiftPlan *p, const EvalLeaf *l)
{
	LeafTerm  **all;
	int			n = 0,
				distinct = 0;

	for (int s = 0; s < p->nsegs; s++)
		n += l->nterms[s];
	if (p->nsegs == 1 || n <= sift_max_expansions)
		return;
	all = palloc(n * sizeof(LeafTerm *));
	n = 0;
	for (int s = 0; s < p->nsegs; s++)
		for (int i = 0; i < l->nterms[s]; i++)
			all[n++] = &l->terms[s][i];
	qsort(all, n, sizeof(LeafTerm *), leaf_term_cmp);
	for (int i = 0; i < n; i++)
		distinct += i == 0 || leaf_term_cmp(&all[i - 1], &all[i]) != 0;
	if (distinct > sift_max_expansions)
		limit_error(l->node);
	pfree(all);
}

static void
resolve_leaf(SiftPlan *p, EvalLeaf *l)
{
	const SiftQNode *node = l->node;
	SiftTermMatcher *m = l->m = node->kind == SQ_TERM ? NULL : sift_matcher_create(node);

	if (m == NULL)
		l->hash = sift_term_hash(node->text, node->textlen);
	l->nterms = MemoryContextAllocZero(p->mcxt, p->nsegs * sizeof(int));
	l->terms = MemoryContextAllocZero(p->mcxt, p->nsegs * sizeof(LeafTerm *));
	for (int s = 0; s < p->nsegs; s++)
	{
		SiftSegment *seg = p->seg[s];
		Expand		e = {.plan = p, .matcher = m};
		SiftTermInfo info;

		if (m == NULL)
		{
			if (sift_seg_lookup(seg, node->text, node->textlen, &info))
				expand_add(&e, node->text, node->textlen, &info);
		}
		else
		{
			const char *lo,
					   *hi;
			int			lolen,
						hilen;

			sift_matcher_bounds(m, &lo, &lolen, &hi, &hilen);
			sift_dict_scan(seg->dict, lo, lolen, hi, hilen, expand_visit, &e);
		}
		sift_sr_release(&seg->sr);
		if (e.over && !p->capped)
			limit_error(node);
		l->nterms[s] = e.n;
		l->terms[s] = e.terms;
	}
	if (!p->capped)
		check_distinct(p, l);
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

static bool
leaf_hits(const EvalLeaf *l, const SiftMutDoc *d)
{
	const char *t = d->terms;

	for (int i = 0; i < d->nterms; t += d->len[i++])
		if (l->m ? sift_matcher_match(l->m, t, d->len[i]) :
			(d->hash[i] == l->hash && d->len[i] == l->node->textlen && memcmp(t, l->node->text, d->len[i]) == 0))
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
key_cmp(const void *a, const void *b)
{
	return pg_cmp_u64(*(const SiftKey *) a, *(const SiftKey *) b);
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
		SiftMutReader *r = sift_mut_open(p->st->index, &p->st->dir.segs[p->mutno[i]], p->mcxt, mut_page_ok, p);
		SiftMutDoc	doc;
		uint64		n,
					sum;

		while (sift_mut_next(r, &doc))
		{
			if (doc.deleted || !doc_matches(p, p->root, &doc))
				continue;
			if (p->nmtids == cap)
			{
				cap = Max(64, 2 * cap);
				p->mtids = p->mtids ? repalloc_huge(p->mtids, cap * sizeof(SiftKey)) :
					MemoryContextAllocHuge(p->mcxt, cap * sizeof(SiftKey));
			}
			p->mtids[p->nmtids++] = sift_key_from_tid(&doc.tid);
		}
		sift_mut_totals(r, &n, &sum);
		ndocs += n;
		sum_dl += sum;
		sift_mut_close(r);
	}
	if (p->nmtids > 1)
	{
		int64		k = 1;

		qsort(p->mtids, p->nmtids, sizeof(SiftKey), key_cmp);
		for (int64 j = 1; j < p->nmtids; j++)
			if (p->mtids[j] != p->mtids[k - 1])
				p->mtids[k++] = p->mtids[j];
		p->nmtids = k;
	}
	bind_stats(p, ndocs, sum_dl);
}

SiftPlan *
sift_plan_bind(SiftIndexState *st, SiftQuery **queries, int nqueries, int flags)
{
	SiftPlan   *p = MemoryContextAllocZero(st->mcxt, sizeof(SiftPlan));
	EvalNode  **roots = MemoryContextAlloc(st->mcxt, nqueries * sizeof(EvalNode *));

	if (flags & SIFT_BIND_SCORE)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("sift: scoring is not implemented yet")));
	p->st = st;
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
	p->seg = MemoryContextAlloc(st->mcxt, Max(p->nsegs, 1) * sizeof(SiftSegment *));
	for (int s = 0; s < p->nsegs; s++)
		p->seg[s] = state_segment(st, p->segno[s]);

	for (int i = 0; i < nqueries; i++)
	{
		roots[i] = relax(p, queries[i]->root, true);
		p->recheck |= queries[i]->needs_recheck;
		if (roots[i]->kind == EN_NOTHING)
		{
			p->root = roots[i];
			bind_stats(p, 0, 0);
			return p;
		}
	}
	p->root = en_make(p, EN_AND, nqueries, roots);
	for (int i = 0; i < p->nleaves; i++)
		resolve_leaf(p, &p->leaves[i]);
	bind_mutable(p);
	return p;
}

const SiftPlanStats *
sift_plan_stats(const SiftPlan *plan)
{
	return &plan->stats;
}

/* ---------- iterator ---------- */

struct SiftMatchIter
{
	SiftPlan   *plan;
	sift_cur  **root;			/* per segment, NULL: no matches */
	const sift_grp **grp;		/* scratch: current group per segment */
	int64		mpos;			/* next mutable match */
	uint32		next_sb;
	uint32		sb;
	int			page;			/* next page of the loaded SB to return */
	uint64		pages[SIFT_PAGEMASK_WORDS];
	uint64	  (*om)[SIFT_OFFMASK_WORDS];	/* merged offset masks by page of the SB */
};

static uint8 *
cursor_buf(MemoryContext cx)
{
	return MemoryContextAllocAligned(cx, SIFT_CUR_BUF, SIFT_SIMD_ALIGN, 0);
}

/* AND / OR of k >= 1 cursors; the single cursor itself for k = 1 */
static sift_cur *
op_cursor(MemoryContext cx, EvalKind kind, sift_cur **kid, int k)
{
	sift_cur   *c;

	if (k == 1)
		return kid[0];
	c = MemoryContextAllocZero(cx, sizeof(sift_cur));
	if (kind == EN_AND)
		sift_cur_and(c, kid, k, cursor_buf(cx));
	else
		sift_cur_or(c, kid, k, cursor_buf(cx));
	return c;
}

static sift_cur *
leaf_cursor(SiftPlan *p, int s, const EvalLeaf *l, MemoryContext cx)
{
	sift_cur  **kid = MemoryContextAlloc(cx, Max(l->nterms[s], 1) * sizeof(sift_cur *));

	for (int i = 0; i < l->nterms[s]; i++)
		kid[i] = sift_post_cursor(sift_post_open(&p->seg[s]->sr, &l->terms[s][i].info, cx));
	return l->nterms[s] == 0 ? NULL : op_cursor(cx, EN_OR, kid, l->nterms[s]);
}

/* Cursor over the segment's matches of n, NULL when there are none */
static sift_cur *
seg_cursor(SiftPlan *p, int s, const EvalNode *n, MemoryContext cx)
{
	sift_cur  **pos,
			  **neg,
			   *c;
	int			npos = 0,
				nneg = 0;

	if (n->kind == EN_NOTHING)
		return NULL;
	if (n->kind == EN_LEAF)
		return leaf_cursor(p, s, &p->leaves[n->leaf], cx);
	pos = MemoryContextAlloc(cx, n->nchild * sizeof(sift_cur *));
	neg = MemoryContextAlloc(cx, n->nchild * sizeof(sift_cur *));
	for (int i = 0; i < n->nchild; i++)
	{
		const EvalNode *k = n->child[i];
		bool		isnot = k->kind == EN_NOT;

		c = seg_cursor(p, s, isnot ? k->child[0] : k, cx);
		if (c == NULL && n->kind == EN_AND && !isnot)
			return NULL;
		if (c != NULL)
			(isnot ? neg : pos)[isnot ? nneg++ : npos++] = c;
	}
	if (npos == 0)
		return NULL;
	c = op_cursor(cx, n->kind, pos, npos);
	if (nneg > 0)
	{
		sift_cur  **kid = MemoryContextAlloc(cx, 2 * sizeof(sift_cur *));
		sift_cur   *an = MemoryContextAllocZero(cx, sizeof(sift_cur));

		kid[0] = c;
		kid[1] = op_cursor(cx, EN_OR, neg, nneg);
		sift_cur_andnot(an, kid, cursor_buf(cx));
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
	for (int s = 0; s < plan->nsegs; s++)
	{
		it->root[s] = seg_cursor(plan, s, plan->root, plan->mcxt);
		sift_sr_release(&plan->seg[s]->sr);
	}
	return it;
}

void
sift_iter_end(SiftMatchIter *it)
{
	for (int s = 0; s < it->plan->nsegs; s++)
		sift_sr_release(&it->plan->seg[s]->sr);
}

/* ORs the live part of one segment's group into the SB buffer */
static void
iter_merge(SiftMatchIter *it, int s, const sift_grp *g)
{
	SiftSegment *seg = it->plan->seg[s];
	const uint8 *off = g->off;
	SiftDocChunk dc;
	bool		dirty = (seg->entry.flags & SIFT_SEG_DIRTY) && sift_seg_docs(seg, g->gid, &dc) && dc.dirty;

	for (int wd = 0; wd < SIFT_PAGEMASK_WORDS; wd++)
		for (uint64 m = g->pages[wd]; m; m &= m - 1, off += g->w)
		{
			int			page = wd * 64 + pg_rightmost_one_pos64(m);
			uint64		mask[SIFT_OFFMASK_WORDS] = {0};
			uint64	   *dst = it->om[page];
			bool		any = false;

			memcpy(mask, off, g->w);
			if (dirty)
			{
				uint64		live[SIFT_OFFMASK_WORDS];

				sift_seg_page_live(seg, &dc, page, live);
				for (int k = 0; k < SIFT_OFFMASK_WORDS; k++)
					mask[k] &= live[k];
			}
			for (int k = 0; k < SIFT_OFFMASK_WORDS; k++)
				any |= mask[k] != 0;
			if (!any)
				continue;
			if (!(it->pages[wd] & (UINT64CONST(1) << (page & 63))))
			{
				memset(dst, 0, SIFT_OFFMASK_WORDS * sizeof(uint64));
				it->pages[wd] |= UINT64CONST(1) << (page & 63);
			}
			for (int k = 0; k < SIFT_OFFMASK_WORDS; k++)
				dst[k] |= mask[k];
		}
	sift_sr_release(&seg->sr);
}

/* Sets the mutable matches of SB sb in the SB buffer */
static void
iter_merge_mutable(SiftMatchIter *it, uint32 sb)
{
	const SiftPlan *p = it->plan;

	for (; it->mpos < p->nmtids && sift_block_sb(sift_key_block(p->mtids[it->mpos])) == sb; it->mpos++)
	{
		SiftKey		key = p->mtids[it->mpos];
		int			page = sift_key_block(key) & (SIFT_SB_PAGES - 1),
					off = (key & 0xFFFF) - 1;

		if (!(it->pages[page / 64] & (UINT64CONST(1) << (page & 63))))
		{
			memset(it->om[page], 0, SIFT_OFFMASK_WORDS * sizeof(uint64));
			it->pages[page / 64] |= UINT64CONST(1) << (page & 63);
		}
		it->om[page][off / 64] |= UINT64CONST(1) << (off & 63);
	}
}

/* Loads the next SB with matches in any segment; false at the end */
static bool
iter_load_sb(SiftMatchIter *it)
{
	uint32		sb;

	for (;;)
	{
		sb = UINT32_MAX;
		for (int s = 0; s < it->plan->nsegs; s++)
		{
			it->grp[s] = it->root[s] ? sift_seek(it->root[s], it->next_sb) : NULL;
			if (it->grp[s] && it->grp[s]->gid < sb)
				sb = it->grp[s]->gid;
		}
		if (it->mpos < it->plan->nmtids)
			sb = Min(sb, sift_block_sb(sift_key_block(it->plan->mtids[it->mpos])));
		if (sb == UINT32_MAX)
			return false;
		CHECK_FOR_INTERRUPTS();
		memset(it->pages, 0, sizeof(it->pages));
		for (int s = 0; s < it->plan->nsegs; s++)
			if (it->grp[s] && it->grp[s]->gid == sb)
				iter_merge(it, s, it->grp[s]);
		iter_merge_mutable(it, sb);
		it->next_sb = sb + 1;
		it->plan->stats.sb_scanned++;
		if (it->pages[0] | it->pages[1] | it->pages[2] | it->pages[3])
			break;
	}
	it->sb = sb;
	it->page = 0;
	return true;
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
	out->recheck = it->plan->recheck;
	out->scores = NULL;
	it->page = page + 1;
	return true;
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
	return 1.0 - none;
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
