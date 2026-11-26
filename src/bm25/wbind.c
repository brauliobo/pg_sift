/*
 * bm25/wbind.c - binds the docs of the write buffers to a plan: their matches, statistics and weights, and the export for parallel scans.
 */
#include <math.h>

#include "core/host.h"

#include "bm25/eval/eval.h"
#include "bm25/eval/int.h"
#include "bm25/wmemo.h"
#include "bm25/kernels/kernels.h"
#include "bm25/wbuf.h"

/*
 * Write buffers are not indexed by term: their docs are matched one by
 * one against the relaxed tree (pages whose bloom rules the tree out are
 * skipped) and the live matching tids join the iterator's output.
 */

static bool
node_may_match(const SiftPlan *p, const EvalNode *n, const uint64 *bloom, int nwords)
{
	check_stack_depth();
	switch (n->kind)
	{
		case EN_NOTHING:
			return false;
		case EN_LEAF:
			return p->leaves[n->leaf].m != NULL || sift_wbuf_bloom_has(bloom, nwords, p->leaves[n->leaf].hash);
		case EN_NOT:
		case EN_ALL:
			return true;
		case EN_AND:
			for (int i = 0; i < n->nchild; i++)
				if (n->child[i]->kind != EN_NOT && !node_may_match(p, n->child[i], bloom, nwords))
					return false;
			return true;
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

/* A scoring bind reads the pages that count in the frequencies, and those of a complement branch, whose matches have no leaf to find them by */
static bool
wbuf_scored_ok(void *arg, const uint64 *bloom, int nwords)
{
	const SiftPlan *p = arg;

	return wbuf_leaf_ok(arg, bloom, nwords) || (p->complement && wbuf_page_ok(arg, bloom, nwords));
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
	check_stack_depth();
	switch (n->kind)
	{
		case EN_NOTHING:
			return false;
		case EN_ALL:
			return true;
		case EN_LEAF:
			return hit[n->leaf];
		case EN_NOT:
			return !doc_matches(p, n->child[0], hit);
		case EN_AND:
			for (int i = 0; i < n->nchild; i++)
				if (!doc_matches(p, n->child[i], hit))
					return false;
			return true;
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

/* The leaves a doc has a term of, and for a scoring plan the best tf of each scoring leaf (touched: those with one, in the order found) */
typedef struct DocHits
{
	bool	   *hit;
	WbufTermEnt **ent;
	uint8	   *best;
	int32	   *touched;
	int			ntouched;
} DocHits;

static void
note_tf(DocHits *h, int leaf, uint8 tf)
{
	if (h->best[leaf] == 0)
		h->touched[h->ntouched++] = leaf;
	h->best[leaf] = Max(h->best[leaf], tf);
}

/*
 * Which leaves the doc has a term of (hit[]), the best tf of every scoring leaf, and one more document for every term of a scoring
 * leaf that it has (deleted docs count like in the statistics): one pass over the terms of the doc, a plain leaf found by its hash in
 * the table of the leaves, and its entry of the table of document frequencies kept in ent[].
 */
static void
doc_hits(SiftPlan *p, const WbufTab *tab, const SiftWbufDoc *d, DocHits *h)
{
	const char *t = d->terms;

	memset(h->hit, 0, Max(p->nleaves, 1));
	for (int i = 0; i < h->ntouched; i++)
		h->best[h->touched[i]] = 0;
	h->ntouched = 0;
	for (int j = 0; j < d->nterms; t += d->len[j++])
	{
		int			li = tab->nexact > 0 ? wbuf_tab_find(p, tab, d->hash[j], t, d->len[j]) : -1;

		for (int k = li; k >= 0; k = tab->next[k])
		{
			h->hit[k] = true;
			if (p->leaves[k].scoring && p->scoring)
			{
				if (h->ent[k] == NULL)
				{
					char		key[SIFT_MAX_TERM_BYTES + 1] = {0};

					memcpy(key, t, d->len[j]);
					h->ent[k] = leaf_wbuf_term(p, &p->leaves[k], key);
				}
				h->ent[k]->df++;
				note_tf(h, k, d->tf[j]);
			}
		}
		for (int w = 0; w < tab->nwild; w++)
		{
			EvalLeaf   *l = &p->leaves[tab->wild[w]];
			bool		counts = l->scoring && p->scoring;

			if ((!counts && h->hit[tab->wild[w]]) || !sift_matcher_match(l->m, t, d->len[j]))
				continue;
			h->hit[tab->wild[w]] = true;
			if (counts)
			{
				leaf_wbuf_count(p, l, t, d->len[j]);
				note_tf(h, tab->wild[w], d->tf[j]);
			}
		}
	}
}

static int
int32_cmp(const void *a, const void *b)
{
	const int32 x = *(const int32 *) a,
				y = *(const int32 *) b;

	return (x > y) - (x < y);
}

/* The lengths of the doc and the best tfs of its scoring leaves, the leaves ascending */
static WbufTfs *
wbuf_tfs(MemoryContext cx, const SiftWbufDoc *d, DocHits *h)
{
	int			nf = d->field_dl ? d->nfields : 0;
	WbufTfs    *r = MemoryContextAlloc(cx, sizeof(WbufTfs) + nf * sizeof(uint16) + h->ntouched * (sizeof(int32) + 1));
	int32	   *leaf = (int32 *) (r + 1);
	uint16	   *fdl = (uint16 *) (leaf + h->ntouched);
	uint8	   *tf = (uint8 *) (fdl + nf);

	qsort(h->touched, h->ntouched, sizeof(int32), int32_cmp);
	for (int i = 0; i < h->ntouched; i++)
		tf[i] = h->best[h->touched[i]];
	*r = (WbufTfs) {.dl = d->dl,.nfields = nf,.n = h->ntouched,.field_dl = d->field_dl ? memcpy(fdl, d->field_dl, nf * sizeof(uint16)) : NULL,
		.leaf = memcpy(leaf, h->touched, h->ntouched * sizeof(int32)),.tf = tf};
	return r;
}

void
bind_wbuf(SiftPlan *p)
{
	uint64		ndocs = 0,
				sum_dl = 0,
				dead_docs = 0,
				dead_dl = 0;
	int64		cap = 0;
	int			nl = Max(p->nleaves, 1);
	DocHits		h = {.hit = palloc(nl * sizeof(bool)),.ent = palloc0(nl * sizeof(WbufTermEnt *)),.best = palloc0(nl),
					 .touched = palloc(nl * sizeof(int32))};
	WbufTab		tab;

	wbuf_tab_build(p, &tab);

	for (int i = 0; i < p->nwbuf && !p->capped; i++)
	{
		/* the lengths of the fields are summed over every doc, not those of the pages the filter keeps */
		SiftWbufReader *r = sift_wbuf_open(p->st->index, &p->st->dir->segs[p->wbufno[i]], p->mcxt,
										 p->scoring && p->fmask ? NULL : p->scoring ? wbuf_scored_ok : wbuf_page_ok, p);
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
			if (p->scoring && p->fmask && doc.field_dl)
			{
				p->wbuf_fdocs++;
				for (int f = 0; f < doc.nfields; f++)
					p->wbuf_fsum[f] += doc.field_dl[f];
			}
			doc_hits(p, &tab, &doc, &h);
			if (!doc_matches(p, p->root, h.hit))
				continue;
			if (p->nmhit == cap)
			{
				cap = Max(64, 2 * cap);
				SIFT_REGROW_IN(p->mcxt, p->mhit, cap * sizeof(WbufHit));
			}
			p->mhit[p->nmhit++] = (WbufHit) {sift_key_from_tid(&doc.tid), 0, p->scoring ? wbuf_tfs(p->mcxt, &doc, &h) : NULL};
		}
		if (!p->no_reads)
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
			else if (p->mhit[j].tfs)
				pfree(p->mhit[j].tfs);
		p->nmhit = k;
	}
	pfree(h.hit);
	pfree(h.ent);
	pfree(h.best);
	pfree(h.touched);
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
					 .wbuf_ndocs = p->wbuf_ndocs,.wbuf_sum_dl = p->wbuf_sum_dl,.seg_fdocs = p->seg_fdocs,.wbuf_fdocs = p->wbuf_fdocs,
					 .nmhit = p->nmhit, .nleaves = p->nleaves};

	memcpy(h.seg_fsum, p->seg_fsum, sizeof(h.seg_fsum));
	memcpy(h.wbuf_fsum, p->wbuf_fsum, sizeof(h.wbuf_fsum));

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
	p->wbuf_fdocs = h->wbuf_fdocs;
	memcpy(p->wbuf_fsum, h->wbuf_fsum, sizeof(p->wbuf_fsum));
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
	bind_field_params(p, (float4) o->k1, (float4) o->b);
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
 * Write buffer matches score at bind from the best tfs bind_wbuf kept: the leaves in canonical order, a phrase as the AND of its terms
 * (the text decides at verify, like for the immutable candidates). All the terms of a leaf weigh the same, and the score of a term grows
 * with its tf, so a leaf scores its best tf (the best of the tfs of its words, for a wide leaf) at its weight.
 */
void
score_wbuf(SiftPlan *p)
{
	for (int64 i = 0; i < p->nmhit; i++)
	{
		WbufHit	   *h = &p->mhit[i];
		const WbufTfs *r = h->tfs;
		float4		knorm = sift_kern_knorm1(&p->bm, r->dl),
					score = 0;

		for (int x = 0; x < r->n; x++)
		{
			const EvalLeaf *l = &p->leaves[r->leaf[x]];

			score += sift_kern_bm25(l->nuniq > 0 ? l->uniq[0]->w : 0, r->tf[x],
									l->field && r->field_dl ? sift_kern_knorm1(&p->bmf[l->field - 1], r->field_dl[l->field - 1]) : knorm);
		}
		h->score = score;
	}
}
