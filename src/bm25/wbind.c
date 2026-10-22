/*
 * bm25/wbind.c - binds the docs of the write buffers to a plan: their matches, statistics and weights, and the export for parallel scans.
 */
#include "core/host.h"

#include <math.h>

#include "bm25/eval.h"
#include "bm25/eval_int.h"
#include "bm25/kernels.h"
#include "bm25/wbuf.h"
#include "bm25/prefix.h"
#include "bm25/segment.h"

/*
 * Write buffers are not indexed by term: their docs are matched one by
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
			return p->leaves[n->leaf].m != NULL || sift_wbuf_bloom_has(bloom, p->leaves[n->leaf].hash);
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
wbuf_page_ok(void *arg, const uint64 *bloom)
{
	SiftPlan   *p = arg;

	return node_may_match(p, p->root, bloom);
}

/* Pages with a term of a scoring leaf: every doc that has one counts in the document frequencies */
static bool
wbuf_leaf_ok(void *arg, const uint64 *bloom)
{
	const SiftPlan *p = arg;

	for (int i = 0; i < p->nleaves; i++)
		if (p->leaves[i].scoring && (p->leaves[i].m != NULL || sift_wbuf_bloom_has(bloom, p->leaves[i].hash)))
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
leaf_hits(const EvalLeaf *l, const SiftWbufDoc *d)
{
	const char *t = d->terms;

	for (int i = 0; i < d->nterms; t += d->len[i++])
		if (leaf_term_hit(l, t, d->len[i], d->hash[i]))
			return true;
	return false;
}

static bool
doc_matches(const SiftPlan *p, const EvalNode *n, const SiftWbufDoc *d)
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
 * One more document for every term of a scoring leaf that the doc has, for the wide term of a wide
 * leaf if it has any word of it (deleted docs count like in the statistics)
 */
static void
doc_count_terms(SiftPlan *p, const SiftWbufDoc *d)
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
				leaf_wbuf_count(p, l, l->wide, l->widelen);
				break;
			}
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

	for (int i = 0; i < p->nwbuf && !p->capped; i++)
	{
		SiftWbufReader *r = sift_wbuf_open(p->st->index, &p->st->dir.segs[p->wbufno[i]], p->mcxt,
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
			if (p->scoring)
				doc_count_terms(p, &doc);
			if (!doc_matches(p, p->root, &doc))
				continue;
			if (p->nmhit == cap)
			{
				cap = Max(64, 2 * cap);
				p->mhit = p->mhit ? repalloc_huge(p->mhit, cap * sizeof(WbufHit)) :
					MemoryContextAllocHuge(p->mcxt, cap * sizeof(WbufHit));
			}
			p->mhit[p->nmhit++] = (WbufHit) {sift_key_from_tid(&doc.tid), 0, p->scoring ? wbufdoc_copy(p->mcxt, &doc) : NULL};
		}
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
	p->wbuf_ndocs = ndocs - dead_docs;
	p->wbuf_sum_dl = sum_dl - dead_dl;
	bind_stats(p, p->wbuf_ndocs, p->wbuf_sum_dl);
}

void *
sift_plan_export(const SiftPlan *p, Size *len)
{
	StringInfoData buf;
	SharedBind	h = {.dir = p->st->dir,.seg_ndocs = p->seg_ndocs,.seg_sum_dl = p->seg_sum_dl,
					 .wbuf_ndocs = p->wbuf_ndocs,.wbuf_sum_dl = p->wbuf_sum_dl,
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
	return &((const SharedBind *) shared)->dir;
}

/* The write buffer half of the bind as the exporting participant had it */
void
import_wbuf(SiftPlan *p, const SharedBind *h)
{
	const char *at = (const char *) (h + 1);

	if (h->nleaves != p->nleaves)
		elog(ERROR, "sift: parallel participants bound different queries");
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
	n += leaf_wbuf_terms(l, all + n);
	qsort(all, n, sizeof(LeafTerm *), leaf_term_cmp);
	l->uniq = all;
	for (int i = 0, j; i < n; i = j)
	{
		double		df = 0;
		float4		w;

		for (j = i; j < n && leaf_term_cmp(&all[i], &all[j]) == 0; j++)
			df += all[j]->info.ldf;
		w = sift_kern_term_weight(l->node->boost, sift_kern_idf(p->stats.ndocs, df), p->bm.k1);
		for (int k = i; k < j; k++)
			all[k]->w = w;
		all[l->nuniq++] = all[i];
	}
}

void
bind_scoring(SiftPlan *p)
{
	const SiftOptions *o = p->st->opts;

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

/* Best term of the leaf in a write buffer doc, weighed like the others; a wide leaf has one term, with the tf of all its words */
static float4
wbuf_leaf_score(const EvalLeaf *l, const SiftWbufDoc *d, float4 knorm)
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
