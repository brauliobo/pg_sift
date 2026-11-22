/*
 * bm25/wbind.c - binds the docs of the write buffers to a plan: their matches, statistics and weights, and the export for parallel scans.
 */
#include <math.h>

#include "core/host.h"

#include "bm25/eval.h"
#include "bm25/eval_int.h"
#include "bm25/wmemo.h"
#include "bm25/kernels.h"
#include "bm25/wbuf.h"

/*
 * Write buffers are not indexed by term: their docs are matched one by
 * one against the relaxed tree (pages whose bloom rules the tree out are
 * skipped) and the live matching tids join the iterator's output.
 */

static bool
node_may_match(const SiftPlan *p, const EvalNode *n, const uint64 *bloom, int nwords)
{
	int			npos = 0;

	switch (n->kind)
	{
		case EN_NOTHING:
			return false;
		case EN_LEAF:
			return p->leaves[n->leaf].m != NULL || sift_wbuf_bloom_has(bloom, nwords, p->leaves[n->leaf].hash);
		case EN_NOT:
			return true;
		case EN_AND:
			for (int i = 0; i < n->nchild; i++)
				if (n->child[i]->kind != EN_NOT)
				{
					npos++;
					if (!node_may_match(p, n->child[i], bloom, nwords))
						return false;
				}
			return npos > 0;
		case EN_OR:
			for (int i = 0; i < n->nchild; i++)
				if (node_may_match(p, n->child[i], bloom, nwords))
					return true;
			return false;
	}
	return false;
}

static bool
wbuf_page_ok(void *arg, const uint64 *bloom, int nwords)
{
	SiftPlan   *p = arg;

	return node_may_match(p, p->root, bloom, nwords);
}

/* Pages with a term of a scoring leaf: every doc that has one counts in the document frequencies */
static bool
wbuf_leaf_ok(void *arg, const uint64 *bloom, int nwords)
{
	const SiftPlan *p = arg;

	for (int i = 0; i < p->nleaves; i++)
		if (p->leaves[i].scoring && (p->leaves[i].m != NULL || sift_wbuf_bloom_has(bloom, nwords, p->leaves[i].hash)))
			return true;
	return false;
}

/* The first position from i of the hash in h[0..n), or n */
static uint32
hash_from(const uint32 *h, uint32 n, uint32 i, uint32 x)
{
#ifdef __SSE2__
	__m128i		vx = _mm_set1_epi32((int) x);

	for (; i + 4 <= n; i += 4)
	{
		int			m = _mm_movemask_ps(_mm_castsi128_ps(_mm_cmpeq_epi32(_mm_loadu_si128((const __m128i *) (h + i)), vx)));

		if (m)
			return i + __builtin_ctz(m);
	}
#endif
	for (; i < n; i++)
		if (h[i] == x)
			break;
	return i;
}

/* The position of a plain leaf's term in the doc and its bytes, or -1: the hashes are compared first, the bytes only of one that is equal */
static int
exact_pos(const EvalLeaf *l, const SiftWbufDoc *d, const char **term)
{
	for (uint32 i = hash_from(d->hash, d->nterms, 0, l->hash); i < d->nterms; i = hash_from(d->hash, d->nterms, i + 1, l->hash))
	{
		const char *t = d->terms + sift_sum_u8(d->len, i);

		if (d->len[i] == l->node->textlen && memcmp(t, l->node->text, d->len[i]) == 0)
		{
			*term = t;
			return (int) i;
		}
	}
	return -1;
}

static bool
doc_matches(const SiftPlan *p, const EvalNode *n, const bool *hit)
{
	int			npos = 0;

	switch (n->kind)
	{
		case EN_NOTHING:
			return false;
		case EN_LEAF:
			return hit[n->leaf];
		case EN_NOT:
			return !doc_matches(p, n->child[0], hit);
		case EN_AND:
			for (int i = 0; i < n->nchild; i++)
			{
				npos += n->child[i]->kind != EN_NOT;
				if (!doc_matches(p, n->child[i], hit))
					return false;
			}
			return npos > 0;
		case EN_OR:
			for (int i = 0; i < n->nchild; i++)
				if (doc_matches(p, n->child[i], hit))
					return true;
			return false;
	}
	return false;
}

static int
hit_cmp(const void *a, const void *b)
{
	return pg_cmp_u64(((const WbufHit *) a)->key, ((const WbufHit *) b)->key);
}

/* The leaf's write buffer term of the zero padded key, created with df 0 */
static WbufTermEnt *
leaf_wbuf_term(SiftPlan *p, EvalLeaf *l, const char *key)
{
	bool		found;
	WbufTermEnt *e;

	if (l->mtab == NULL)
	{
		HASHCTL		ctl = {.keysize = SIFT_MAX_TERM_BYTES + 1, .entrysize = sizeof(WbufTermEnt), .hcxt = p->mcxt};

		l->mtab = hash_create("sift write buffer terms", 64, &ctl, HASH_ELEM | HASH_BLOBS | HASH_CONTEXT);
	}
	e = hash_search(l->mtab, key, HASH_ENTER, &found);
	if (!found)
		e->df = 0;
	return e;
}

static void
leaf_wbuf_count(SiftPlan *p, EvalLeaf *l, const char *t, int len)
{
	char		key[SIFT_MAX_TERM_BYTES + 1] = {0};

	memcpy(key, t, len);
	leaf_wbuf_term(p, l, key)->df++;
}

/*
 * Which leaves the doc has a term of (hit[]), and one more document for every term of a scoring leaf that it has
 * (deleted docs count like in the statistics). A plain leaf is found by its hash, and its entry of the table of
 * document frequencies is kept in ent[]: a doc costs a pass over its hashes for each leaf, not a hash table lookup for every hit.
 */
static void
doc_hits(SiftPlan *p, const SiftWbufDoc *d, bool *hit, WbufTermEnt **ent)
{
	for (int i = 0; i < p->nleaves; i++)
	{
		EvalLeaf   *l = &p->leaves[i];
		const char *t = d->terms;

		hit[i] = false;
		if (l->m == NULL)
		{
			if (exact_pos(l, d, &t) < 0)
				continue;
			hit[i] = true;
			if (l->scoring && p->scoring)
			{
				if (ent[i] == NULL)
				{
					char		key[SIFT_MAX_TERM_BYTES + 1] = {0};

					memcpy(key, t, l->node->textlen);
					ent[i] = leaf_wbuf_term(p, l, key);
				}
				ent[i]->df++;
			}
			continue;
		}
		for (int j = 0; j < d->nterms; t += d->len[j++])
		{
			if (!sift_matcher_match(l->m, t, d->len[j]))
				continue;
			hit[i] = true;
			if (!(l->scoring && p->scoring))
				break;
			leaf_wbuf_count(p, l, t, d->len[j]);
		}
	}
}

static SiftWbufDoc *
wbufdoc_copy(MemoryContext cx, const SiftWbufDoc *d)
{
	size_t		tl = 0;
	SiftWbufDoc *c;
	uint32	   *hash;
	uint8	   *tf,
			   *len;

	for (int i = 0; i < d->nterms; i++)
		tl += d->len[i];
	c = MemoryContextAlloc(cx, sizeof(SiftWbufDoc) + d->nterms * (sizeof(uint32) + 2) + tl);
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

void
bind_wbuf(SiftPlan *p)
{
	uint64		ndocs = 0,
				sum_dl = 0,
				dead_docs = 0,
				dead_dl = 0;
	int64		cap = 0;
	bool	   *hit = palloc(Max(p->nleaves, 1) * sizeof(bool));
	WbufTermEnt **ent = palloc0(Max(p->nleaves, 1) * sizeof(WbufTermEnt *));

	for (int i = 0; i < p->nwbuf && !p->capped; i++)
	{
		SiftWbufReader *r = sift_wbuf_open(p->st->index, &p->st->dir->segs[p->wbufno[i]], p->mcxt,
										 p->scoring ? wbuf_leaf_ok : wbuf_page_ok, p);
		SiftWbufDoc	doc;
		uint64		n,
					sum,
					seen = 0;

		while (sift_wbuf_next(r, &doc))
		{
			if ((++seen & 1023) == 0)
				CHECK_FOR_INTERRUPTS();
			if (doc.deleted)
			{
				dead_docs++;
				dead_dl += doc.dl;
				continue;
			}
			doc_hits(p, &doc, hit, ent);
			if (!doc_matches(p, p->root, hit))
				continue;
			if (p->nmhit == cap)
			{
				cap = Max(64, 2 * cap);
				SIFT_REGROW_HUGE_IN(p->mcxt, p->mhit, cap * sizeof(WbufHit));
			}
			p->mhit[p->nmhit++] = (WbufHit) {sift_key_from_tid(&doc.tid), 0, p->scoring ? wbufdoc_copy(p->mcxt, &doc) : NULL};
		}
		sift_wbuf_note_walked(r);
		sift_wbuf_totals(r, &n, &sum);
		ndocs += n;
		sum_dl += sum;
		sift_wbuf_close(r);
	}
	if (p->nmhit > 1)
	{
		int64		k = 1;

		qsort(p->mhit, p->nmhit, sizeof(WbufHit), hit_cmp);
		for (int64 j = 1; j < p->nmhit; j++)
			if (p->mhit[j].key != p->mhit[k - 1].key)
				p->mhit[k++] = p->mhit[j];
		p->nmhit = k;
	}
	pfree(hit);
	pfree(ent);
	p->wbuf_ndocs = ndocs - dead_docs;
	p->wbuf_sum_dl = sum_dl - dead_dl;
	bind_stats(p, p->wbuf_ndocs, p->wbuf_sum_dl);
}

void *
sift_plan_export(const SiftPlan *p, Size *len)
{
	StringInfoData buf;
	SharedBind	h = {.dir_len = sift_dir_bytes(p->st->dir),.seg_ndocs = p->seg_ndocs,.seg_sum_dl = p->seg_sum_dl,
					 .wbuf_ndocs = p->wbuf_ndocs,.wbuf_sum_dl = p->wbuf_sum_dl,
					 .nmhit = p->nmhit, .nleaves = p->nleaves};

	initStringInfo(&buf);
	appendBinaryStringInfo(&buf, &h, sizeof(h));
	appendBinaryStringInfo(&buf, p->st->dir, h.dir_len);
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
		WbufTermEnt *e;

		appendBinaryStringInfo(&buf, &n, sizeof(n));
		if (n > 0)
		{
			hash_seq_init(&seq, l->mtab);
			while ((e = hash_seq_search(&seq)) != NULL)
				appendBinaryStringInfo(&buf, e, sizeof(WbufTermEnt));
		}
	}
	*len = buf.len;
	return buf.data;
}

const SiftDirSnapshot *
sift_shared_dir(const void *shared)
{
	return (const SiftDirSnapshot *) ((const char *) shared + sizeof(SharedBind));
}

/* The bind of a scan that exports only its directory: the header of a bind with nothing in it, then the directory */
void *
sift_dir_export(const SiftDirSnapshot *dir, Size *len)
{
	SharedBind	h = {.dir_len = sift_dir_bytes(dir)};
	char	   *blob = palloc(sizeof(h) + h.dir_len);

	memcpy(blob, &h, sizeof(h));
	memcpy(blob + sizeof(h), dir, h.dir_len);
	*len = sizeof(h) + h.dir_len;
	return blob;
}

/* The write buffer half of the bind as the exporting participant had it */
void
import_wbuf(SiftPlan *p, const SharedBind *h)
{
	const char *at = (const char *) (h + 1) + h->dir_len;

	SIFT_INVARIANT(h->nleaves == p->nleaves, "sift: parallel participants bound different queries");
	p->wbuf_ndocs = h->wbuf_ndocs;
	p->wbuf_sum_dl = h->wbuf_sum_dl;
	p->nmhit = h->nmhit;
	p->mhit = MemoryContextAllocHuge(p->mcxt, Max(h->nmhit, 1) * sizeof(WbufHit));
	for (int64 i = 0; i < h->nmhit; i++, at += sizeof(SharedHit))
	{
		SharedHit	sh;

		memcpy(&sh, at, sizeof(sh));
		p->mhit[i] = (WbufHit) {sh.key, sh.score, NULL};
	}
	for (int i = 0; i < p->nleaves; i++)
	{
		int32		n;

		memcpy(&n, at, sizeof(n));
		at += sizeof(n);
		for (int32 j = 0; j < n; j++, at += sizeof(WbufTermEnt))
		{
			WbufTermEnt	e;

			memcpy(&e, at, sizeof(e));
			leaf_wbuf_term(p, &p->leaves[i], e.term)->df = e.df;
		}
	}
	bind_stats(p, h->wbuf_ndocs, h->wbuf_sum_dl);
}

/* The terms of a scoring leaf that only write buffer docs have weigh like the others: by df over the whole index */
static int
leaf_wbuf_terms(EvalLeaf *l, LeafTerm **out)
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

/*
 * The df a leaf is weighed by: its terms' dfs as independent chances of a document having any of them, never below the
 * largest. Only the distinct live terms count, so it does not depend on how many of them the expansion limit keeps.
 */
static double
leaf_blended_df(double ndocs, const double *ldf, int n)
{
	double		max_df = 0,
				none = 0,
				df;

	if (n == 1 || ndocs <= 0)
		return n == 1 ? ldf[0] : 0;
	for (int i = 0; i < n; i++)
	{
		if (ldf[i] >= ndocs)
			return ndocs;
		max_df = Max(max_df, ldf[i]);
		none += log(1.0 - ldf[i] / ndocs);
	}
	df = ndocs * (1.0 - exp(none));
	return Min(Max(df, max_df), ndocs);
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

/* The terms a deferred leaf keeps (at most WIDE_TOP_MAX): the ones with the largest bounds, then the largest df, then the smallest bytes */
typedef struct TopTerm
{
	char	   *term;
	int			len;
	uint8		mb;				/* the largest bound over the segments */
	double		ldf;
	SiftTermInfo *info;			/* [segment] the term's entry, ldf 0 where it has none */
} TopTerm;

typedef struct TopTerms
{
	int			e;
	int			n;
	TopTerm	   *t;
} TopTerms;

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

/* A term the leaf does not keep: the segments' bounds of the terms left out (in[s]: the segment has it, none: info[s].ldf is 0 where it has not) */
static void
top_left_out(EvalLeaf *l, const SiftTermInfo *info, const bool *in, int nsegs)
{
	for (int s = 0; s < nsegs; s++)
		if ((in == NULL || in[s]) && info[s].ldf > 0)
			l->tail_mb[s] = Max(l->tail_mb[s], info[s].max_bound);
}

static void
top_set(SiftPlan *p, TopTerm *t, const char *term, int len, uint8 mb, double ldf, const SiftTermInfo *info, const bool *in)
{
	t->term = MemoryContextAlloc(p->mcxt, len);
	memcpy(t->term, term, len);
	t->len = len;
	t->mb = mb;
	t->ldf = ldf;
	t->info = MemoryContextAllocZero(p->mcxt, p->nsegs * sizeof(SiftTermInfo));
	for (int s = 0; s < p->nsegs; s++)
		if (in[s])
		{
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

/* A distinct term of the leaf with a posting in some segment: kept when its bound is among the largest, else left out (in[s]: the segment has it, info[s] its entry there) */
static void
top_consider(SiftPlan *p, EvalLeaf *l, TopTerms *tt, const char *term, int len, double ldf, const SiftTermInfo *info, const bool *in)
{
	uint8		mb = 0;
	bool		any = false;

	for (int s = 0; s < p->nsegs; s++)
		if (in[s])
		{
			mb = Max(mb, info[s].max_bound);
			l->all_mb[s] = Max(l->all_mb[s], info[s].max_bound);
			any = true;
		}
	if (!any)
		return;
	if (tt->n < tt->e)
	{
		top_set(p, &tt->t[tt->n], term, len, mb, ldf, info, in);
		top_sift_up(tt->t, tt->n++);
	}
	else if (top_beats(mb, ldf, term, len, &tt->t[0]))
	{
		top_left_out(l, tt->t[0].info, NULL, p->nsegs);
		top_free(p, &tt->t[0]);
		top_set(p, &tt->t[0], term, len, mb, ldf, info, in);
		top_sift_down(tt->t, tt->n, 0);
	}
	else
		top_left_out(l, info, in, p->nsegs);
}

/* The kept terms as the leaf's terms in each segment, ascending, weighed w */
static void
top_install(SiftPlan *p, EvalLeaf *l, TopTerms *tt, float4 w)
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
	bool		in = true;
	SiftTermInfo info;

	for (; ts->more; stream_next(ts, l, hi, hilen))
	{
		if (ts->ldf <= 0)
			continue;
		info = ts->info;
		if (tt)
			top_consider(p, l, tt, ts->term, ts->len, ts->ldf, &info, &in);
		wide_acc(a, ts->ldf);
	}
}

/* The distinct terms over the segments and the write buffer: the minimum of the streams, the df of it over the streams that hold it */
static void
wide_merge(SiftPlan *p, EvalLeaf *l, TopTerms *tt, TermStream *ts, const char *hi, int hilen, WideAcc *acc)
{
	int			ns = p->nsegs + 1;
	SiftTermInfo *cur_info = palloc(ns * sizeof(SiftTermInfo));
	bool	   *cur_in = palloc(ns * sizeof(bool)),
			   *cur_eq = palloc(ns * sizeof(bool));

	for (;;)
	{
		const TermStream *min = NULL;
		double		df = 0;
		char		cur[SIFT_MAX_TERM_BYTES];
		int			curlen;

		for (int i = 0; i < ns; i++)
			if (ts[i].more && (min == NULL || sift_term_cmp(ts[i].term, ts[i].len, min->term, min->len) < 0))
				min = &ts[i];
		if (min == NULL)
			break;
		memcpy(cur, min->term, curlen = min->len);
		for (int i = 0; i < ns; i++)
			if (ts[i].more && sift_term_cmp(ts[i].term, ts[i].len, cur, curlen) == 0)
			{
				df += ts[i].ldf;
				cur_eq[i] = true;
				cur_in[i] = i < p->nsegs && ts[i].ldf > 0;
				cur_info[i] = ts[i].info;
			}
			else
				cur_eq[i] = cur_in[i] = false;
		if (df > 0 && tt)
			top_consider(p, l, tt, cur, curlen, df, cur_info, cur_in);
		for (int i = 0; i < ns; i++)
			if (cur_eq[i])
				stream_next(&ts[i], l, i < p->nsegs ? hi : NULL, hilen);
		if (df > 0)
			wide_acc(acc, df);
	}
	pfree(cur_info);
	pfree(cur_in);
	pfree(cur_eq);
}

/*
 * The blended df of a wide leaf, from a merge of the dictionaries of the segments and the terms of the write buffer in byte order, one distinct term at a
 * time: the terms are not kept, and the accumulation is that of leaf_blended_df over the distinct terms in the same order.
 */
static double
wide_blended_df(SiftPlan *p, EvalLeaf *l, TopTerms *tt)
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
	ts[p->nsegs].nwb = leaf_wbuf_terms(l, wb);
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

/* The terms of a leaf over all segments and the write buffer, ascending, with the df of each over the index */
static int
leaf_all_terms(SiftPlan *p, EvalLeaf *l, LeafTerm ***out)
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
	n += leaf_wbuf_terms(l, all + n);
	qsort(all, n, sizeof(LeafTerm *), leaf_term_cmp);
	*out = all;
	return n;
}

/* The top terms of a deferred leaf and the blended df, from the dictionaries; the leaf reads no term before this */
static double
weigh_top(SiftPlan *p, EvalLeaf *l, TopTerms *tt)
{
	tt->e = Min(Max(sift_max_expansions, 1), WIDE_TOP_MAX);
	tt->t = palloc0(tt->e * sizeof(TopTerm));
	l->tail_mb = MemoryContextAllocZero(p->mcxt, Max(p->nsegs, 1));
	l->all_mb = MemoryContextAllocZero(p->mcxt, Max(p->nsegs, 1));
	return wide_blended_df(p, l, tt);
}

/* The kept terms of the deferred leaf (top_install) once its weight is known */
static void
install_top(SiftPlan *p, EvalLeaf *l, TopTerms *tt)
{
	top_install(p, l, tt, l->wide_w);
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
			weigh_top(p, l, &tt);
			install_top(p, l, &tt);
		}
	}
}

/*
 * All the terms of a leaf weigh the same, by its blended df, and a document scores the best tf of any of them: the same
 * scores whether the leaf stays a list of terms or reads as the one wide term.
 */
static void
weigh_leaf(SiftPlan *p, EvalLeaf *l)
{
	LeafTerm  **all = NULL;
	int			n = 0,
				nuniq = 0;
	double	   *ldf;
	double		df_blend;
	float4		w;
	TopTerms	tt = {0};

	if (l->wide)
	{
		if (!l->deferred)
			df_blend = wide_blended_df(p, l, NULL);
		else if (!sift_wide_memo_warm(p, l, &df_blend))
		{
			df_blend = weigh_top(p, l, &tt);
			sift_wide_memo_store(p, l, df_blend, l->all_mb);
		}
	}
	else
	{
		n = leaf_all_terms(p, l, &all);
		ldf = palloc(Max(n, 1) * sizeof(double));
		for (int i = 0, j; i < n; i = j)
		{
			double		df = 0;

			for (j = i; j < n && leaf_term_cmp(&all[i], &all[j]) == 0; j++)
				df += all[j]->info.ldf;
			if (df > 0)
				ldf[nuniq++] = df;
		}
		df_blend = leaf_blended_df(p->stats.ndocs, ldf, nuniq);
		pfree(ldf);
	}
	w = sift_kern_term_weight(l->node->boost, sift_kern_idf(p->stats.ndocs, df_blend), p->bm.k1);
	if (l->wide)
	{
		LeafTerm   *t = MemoryContextAllocZero(p->mcxt, sizeof(LeafTerm));

		*t = (LeafTerm) {.term = l->wide, .len = l->widelen, .w = w};
		l->wide_w = w;
		if (l->deferred)
		{
			if (!l->warm)
				top_install(p, l, &tt, w);
		}
		else
			for (int s = 0; s < p->nsegs; s++)
				if (l->nterms[s])
					l->terms[s][0].w = w;
		l->uniq = MemoryContextAlloc(p->mcxt, sizeof(LeafTerm *));
		l->uniq[l->nuniq++] = t;
		return;
	}
	l->uniq = all;
	for (int i = 0, j; i < n; i = j)
	{
		for (j = i; j < n && leaf_term_cmp(&all[i], &all[j]) == 0; j++)
			all[j]->w = w;
		all[l->nuniq++] = all[i];
	}
}

void
bind_scoring(SiftPlan *p)
{
	const SiftOptions *o = sift_index_options(p->st->index);

	sift_kern_bm25_init(&p->bm, (float4) o->k1, (float4) o->b, p->stats.avgdl > 0 ? (float4) p->stats.avgdl : 1.0f);
	for (int i = 0; i < p->nleaves; i++)
		if (p->leaves[i].scoring)
			weigh_leaf(p, &p->leaves[i]);
}

/* The weighed term of the leaf with these bytes */
const LeafTerm *
leaf_weighed(const EvalLeaf *l, const char *term, int len)
{
	LeafTerm	key = {.term = (char *) term,.len = len},
			   *k = &key;

	return *(LeafTerm **) bsearch(&k, l->uniq, l->nuniq, sizeof(LeafTerm *), leaf_term_cmp);
}

/* Best term of the leaf in a write buffer doc, weighed like the others; a wide leaf has one term, with the best tf of its words */
static float4
wbuf_leaf_score(const EvalLeaf *l, const SiftWbufDoc *d, float4 knorm)
{
	const char *t = d->terms;
	float4		best = 0;
	int			wtf = 0;
	int			j;

	if (l->m == NULL)
		return (j = exact_pos(l, d, &t)) < 0 ? 0 : sift_kern_bm25(leaf_weighed(l, t, d->len[j])->w, d->tf[j], knorm);
	for (j = 0; j < d->nterms; t += d->len[j++])
		if (sift_matcher_match(l->m, t, d->len[j]))
		{
			if (l->wide)
				wtf = Max(wtf, d->tf[j]);
			else
				best = Max(best, sift_kern_bm25(leaf_weighed(l, t, d->len[j])->w, d->tf[j], knorm));
		}
	return wtf > 0 ? sift_kern_bm25(leaf_weighed(l, l->wide, l->widelen)->w, Min(wtf, SIFT_MAX_TF), knorm) : best;
}

/*
 * Write buffer matches score at bind from their records: the leaves in canonical order, a phrase as the AND
 * of its terms (the text decides at verify, like for the immutable candidates).
 */
void
score_wbuf(SiftPlan *p)
{
	for (int64 i = 0; i < p->nmhit; i++)
	{
		WbufHit	   *h = &p->mhit[i];
		float4		knorm = sift_kern_knorm1(&p->bm, h->doc->dl),
					score = 0;

		for (int j = 0; j < p->nleaves; j++)
			if (p->leaves[j].scoring)
				score += wbuf_leaf_score(&p->leaves[j], h->doc, knorm);
		h->score = score;
	}
}
