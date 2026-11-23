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

/*
 * The plain leaves of the plan by their term, for the pass over the terms of a doc: head[] is an open addressed table of the first leaf
 * with each distinct term, next[] chains the leaves that share it; the other leaves (matchers) are in wild[]
 */
typedef struct WbufTab
{
	int32	   *head;
	int32	   *next;
	uint32		mask;
	int		   *wild;
	int			nwild;
	int			nexact;
} WbufTab;

static bool
leaf_has_term(const EvalLeaf *l, uint32 hash, const char *t, int len)
{
	return l->hash == hash && l->node->textlen == len && memcmp(l->node->text, t, len) == 0;
}

static void
wbuf_tab_build(const SiftPlan *p, WbufTab *tab)
{
	uint32		size = 8;

	*tab = (WbufTab) {.next = palloc(Max(p->nleaves, 1) * sizeof(int32)),.wild = palloc(Max(p->nleaves, 1) * sizeof(int))};
	for (int i = 0; i < p->nleaves; i++)
		tab->nexact += p->leaves[i].m == NULL;
	while (size < 2 * (uint32) tab->nexact)
		size *= 2;
	tab->mask = size - 1;
	tab->head = palloc0(size * sizeof(int32));
	for (int i = 0; i < p->nleaves; i++)
	{
		const EvalLeaf *l = &p->leaves[i];
		uint32		s;

		if (l->m != NULL)
		{
			tab->wild[tab->nwild++] = i;
			continue;
		}
		for (s = l->hash & tab->mask; tab->head[s]; s = (s + 1) & tab->mask)
			if (leaf_has_term(&p->leaves[tab->head[s] - 1], l->hash, l->node->text, l->node->textlen))
				break;
		tab->next[i] = tab->head[s] ? tab->head[s] - 1 : -1;
		tab->head[s] = i + 1;
	}
}

static void
wbuf_tab_free(WbufTab *tab)
{
	pfree(tab->head);
	pfree(tab->next);
	pfree(tab->wild);
}

/* The first leaf of the plain leaves with the term of the doc at hash/t/len, or -1 */
static int
wbuf_tab_find(const SiftPlan *p, const WbufTab *tab, uint32 hash, const char *t, int len)
{
	for (uint32 s = hash & tab->mask; tab->head[s]; s = (s + 1) & tab->mask)
	{
		int			li = tab->head[s] - 1;

		if (leaf_has_term(&p->leaves[li], hash, t, len))
			return li;
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
 * Which leaves the doc has a term of (hit[]), and one more document for every term of a scoring leaf that it has (deleted docs
 * count like in the statistics): one pass over the terms of the doc, a plain leaf found by its hash in the table of the leaves, and
 * its entry of the table of document frequencies kept in ent[].
 */
static void
doc_hits(SiftPlan *p, const WbufTab *tab, const SiftWbufDoc *d, bool *hit, WbufTermEnt **ent)
{
	const char *t = d->terms;

	memset(hit, 0, Max(p->nleaves, 1));
	for (int j = 0; j < d->nterms; t += d->len[j++])
	{
		int			li = tab->nexact > 0 ? wbuf_tab_find(p, tab, d->hash[j], t, d->len[j]) : -1;

		for (int k = li; k >= 0; k = tab->next[k])
		{
			hit[k] = true;
			if (p->leaves[k].scoring && p->scoring)
			{
				if (ent[k] == NULL)
				{
					char		key[SIFT_MAX_TERM_BYTES + 1] = {0};

					memcpy(key, t, d->len[j]);
					ent[k] = leaf_wbuf_term(p, &p->leaves[k], key);
				}
				ent[k]->df++;
			}
		}
		for (int w = 0; w < tab->nwild; w++)
		{
			EvalLeaf   *l = &p->leaves[tab->wild[w]];
			bool		counts = l->scoring && p->scoring;

			if ((!counts && hit[tab->wild[w]]) || !sift_matcher_match(l->m, t, d->len[j]))
				continue;
			hit[tab->wild[w]] = true;
			if (counts)
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

/* MEM[MEM-BM25-04]: wbind wbufdoc_copy + mhit + plan_export (unbudgeted deep-copy + huge mhit + unbounded StringInfo) */
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
	WbufTab		tab;

	wbuf_tab_build(p, &tab);

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
			doc_hits(p, &tab, &doc, hit, ent);
			if (!doc_matches(p, p->root, hit))
				continue;
			if (p->nmhit == cap)
			{
				cap = Max(64, 2 * cap);
				SIFT_REGROW_IN(p->mcxt, p->mhit, cap * sizeof(WbufHit));
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
			else if (p->mhit[j].doc)
				pfree(p->mhit[j].doc);
		p->nmhit = k;
	}
	pfree(hit);
	pfree(ent);
	wbuf_tab_free(&tab);
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
	p->mhit = MemoryContextAlloc(p->mcxt, Max(h->nmhit, 1) * sizeof(WbufHit));
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
	n += tail_wbuf_terms(l, all + n);
	qsort(all, n, sizeof(LeafTerm *), leaf_term_cmp);
	*out = all;
	return n;
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
			df_blend = tail_wide_blended_df(p, l, NULL);
		else if (!sift_wide_memo_warm(p, l, &df_blend))
		{
			df_blend = tail_weigh_top(p, l, &tt);
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
				tail_top_install(p, l, &tt, w);
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

/*
 * Write buffer matches score at bind from their records: the leaves in canonical order, a phrase as the AND of its terms (the text
 * decides at verify, like for the immutable candidates). All the terms of a leaf weigh the same, and the score of a term grows with
 * its tf, so a leaf scores its best tf (the best of the tfs of its words, for a wide leaf) at its weight: one pass over the terms
 * of the doc finds the best tf of every leaf.
 */
void
score_wbuf(SiftPlan *p)
{
	WbufTab		tab;
	uint8	   *best = palloc(Max(p->nleaves, 1));
	float4	   *w = palloc(Max(p->nleaves, 1) * sizeof(float4));

	wbuf_tab_build(p, &tab);
	for (int j = 0; j < p->nleaves; j++)
		w[j] = p->leaves[j].nuniq > 0 ? p->leaves[j].uniq[0]->w : 0;
	for (int64 i = 0; i < p->nmhit; i++)
	{
		WbufHit	   *h = &p->mhit[i];
		const SiftWbufDoc *d = h->doc;
		const char *t = d->terms;
		float4		knorm = sift_kern_knorm1(&p->bm, d->dl),
					score = 0;

		memset(best, 0, Max(p->nleaves, 1));
		for (int j = 0; j < d->nterms; t += d->len[j++])
		{
			int			li = tab.nexact > 0 ? wbuf_tab_find(p, &tab, d->hash[j], t, d->len[j]) : -1;

			for (int k = li; k >= 0; k = tab.next[k])
				best[k] = Max(best[k], d->tf[j]);
			for (int x = 0; x < tab.nwild; x++)
				if (p->leaves[tab.wild[x]].scoring && sift_matcher_match(p->leaves[tab.wild[x]].m, t, d->len[j]))
					best[tab.wild[x]] = Max(best[tab.wild[x]], d->tf[j]);
		}
		for (int j = 0; j < p->nleaves; j++)
			if (p->leaves[j].scoring && best[j] > 0)
				score += sift_kern_bm25(w[j], best[j], knorm);
		h->score = score;
	}
	wbuf_tab_free(&tab);
	pfree(best);
	pfree(w);
}
