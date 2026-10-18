/*
 * query.c - queries over the segments and the live segment.
 *
 * Freshness. A query first reads the queue rows the segments do not hold yet (the host's `queue`), so committed DML is
 * seen at once: the rows of the queue become a segment of their own, built in memory by the same writer as a drain and
 * ranked with the others (scores are those the index will have after the drain), and the documents they retire (the
 * previous version of a row that was updated or deleted) are dropped from the results. The live segment is cached per
 * index while the queue does not change. A query that finds the index moved under it (a drain published the rows it
 * had read) starts again.
 * Exactness. A top-k asks the host which of its keys still exist in the base table and takes the next candidates for the
 * ones that do not, so a row removed without a trigger (TRUNCATE, an ON DELETE CASCADE) does not shorten a LIMIT. A phrase
 * the index cannot decide (under OR or NOT, or in a segment without positions) is checked on the text of the row.
 * A query holds the index read-locked from the moment it takes its view: writers swap in what they prepared under the
 * write lock, so the segments and tables a query reads never change under it.
 */
#include "ix.h"

#include <unistd.h>

#include "sift_analyze.h"
#include "sift_bitmap.h"
#include "sift_cmp.h"
#include "sift_eval.h"
#include "sift_kernels.h"
#include "sift_runs.h"

#define DELTA_BUDGET	((Size) 256 << 20)
#define DELTA_MAX_DOCS	500000
#define RETRIES			8

typedef struct Qry
{
	SiftIdx    *ix;
	const SxHost *h;
	const char *q;
	int			qlen;
	int			k;
	const SxIdSet *flt;
	SxHit	   *hits;
	int			n;
	int64		count;
	int64	   *pks;
	int64		npks,
				cappks;
	Relation	rel;				/* the view */
	SiftRel    *sr;
	SiftDirSnapshot snap;
	Delta	   *dl;
	int64		dlfrom;				/* docids above belong to the live segment */
	int64	   *fdocs;				/* documents the filter allows, ascending */
	size_t		nfdocs;
	SiftFilter *filter;
} Qry;

/* ------------------------------------------------ the live segment */

typedef struct LastRow				/* the last queue row of a key */
{
	int64		pk;
	int64		seq;
	char		op;
} LastRow;

static uint64
events_sig(const SxEvent *ev, size_t n)
{
	uint64		h = 1469598103934665603ULL;

	for (size_t i = 0; i < n; i++)
	{
		h = (h ^ (uint64) ev[i].seq) * 1099511628211ULL;
		h = (h ^ (uint64) ev[i].pk) * 1099511628211ULL;
		h = (h ^ (uint64) ev[i].op) * 1099511628211ULL;
	}
	return h ^ n;
}

static int
cmp_group(const void *a, const void *b)
{
	const LastRow *x = a,
			   *y = b;

	if (x->pk != y->pk)
		return x->pk < y->pk ? -1 : 1;
	return (x->seq > y->seq) - (x->seq < y->seq);
}

/* One entry per key with the last of its rows: what the queue says the row is now */
static size_t
last_rows(const SxEvent *ev, size_t n, LastRow **out)
{
	LastRow	   *g = malloc(n * sizeof(LastRow));
	size_t		ng = 0;

	for (size_t i = 0; i < n; i++)
		g[i] = (LastRow) {ev[i].pk, ev[i].seq, ev[i].op};
	qsort(g, n, sizeof(LastRow), cmp_group);
	for (size_t i = 0; i < n; i++)
		if (i + 1 == n || g[i + 1].pk != g[i].pk)
			g[ng++] = g[i];
	*out = g;
	return ng;
}

static Delta *
delta_cached(SiftIdx *ix, uint64 sig, int64 after, uint64 epoch)
{
	Delta	   *d;

	pthread_mutex_lock(&ix->dmu);
	d = ix->delta;
	if (d && d->sig == sig && d->after == after && d->epoch == epoch)
		__atomic_add_fetch(&d->refs, 1, __ATOMIC_ACQ_REL);
	else
		d = NULL;
	pthread_mutex_unlock(&ix->dmu);
	return d;
}

/* Analyzes the documents of the delta and maps them as a segment in memory */
static void
delta_segment(SiftIdx *ix, Delta *d)
{
	static uint64 counter;
	MemoryContext cx = op_begin(),
				old = MemoryContextSwitchTo(cx);
	SiftAnalyzerConfig cfg;
	SiftAnalyzer *an;
	SiftInvBuf *inv;
	SiftWriteTarget *wt;
	SiftScoreRef ref = {ix->sr->opts.k1, ix->sr->opts.b, 0};
	bool		pos = ix->sr->opts.positions;

	sift_analyzer_config_from_options(&ix->sr->opts, &cfg);
	an = sift_analyzer_create(&cfg, cx);
	sift_analyzer_set_positions(an, pos);
	inv = sift_inv_create(DELTA_BUDGET, cx, pos);
	for (size_t i = 0; i < d->nd; i++)
	{
		SiftTermFreq *terms;
		uint32		dl;
		ItemPointerData tid;
		int			n;

		sift_key_to_tid(sift_docid_key(ix->sr, d->dids[i]), &tid);
		n = sift_analyze_doc(an, d->text[i], d->tlen[i], &terms, &dl);
		sift_inv_add(inv, &tid, terms, n, dl);
		sift_analyzer_reset(an);
	}
	wt = sift_wt_begin_at(ix->sr, SIFT_DELTA_SLOT, true);
	sift_inv_write(inv, wt, ((uint64) 0xFFFF << 48) | __atomic_add_fetch(&counter, 1, __ATOMIC_RELAXED), &ref, &d->e);
	sift_wt_finish(wt);
	d->g = sift_map_fd(sift_wt_fd(wt), SIFT_DELTA_SLOT, &d->blocks);
	close(sift_wt_fd(wt));
	MemoryContextSwitchTo(old);
	op_end(cx);
}

typedef struct DocRef
{
	int64		did;
	int64		pk;
	char	   *text;
	size_t		len;
} DocRef;

static int
cmp_doc(const void *a, const void *b)
{
	return sift_cmp_i64(&((const DocRef *) a)->did, &((const DocRef *) b)->did);
}

/* The documents the queue rows add (those whose row has text) in ascending docids, and the segment documents they retire */
static void
delta_fill(SiftIdx *ix, Delta *d, const LastRow *g, size_t ng, size_t nc, SxRow *rows)
{
	DocRef	   *docs = malloc((nc + 1) * sizeof(DocRef));
	size_t		next = 0;

	d->dead = malloc(ng * sizeof(int64));
	for (size_t i = 0; i < ng; i++)
	{
		int64		doc = latest_get(ix, g[i].pk);

		if (doc)
			d->dead[d->ndead++] = doc;
		if (g[i].op == 'I' && rows[next++].text)
			docs[d->nd++] = (DocRef) {g[i].seq + ix->man.docbase, g[i].pk, rows[next - 1].text, rows[next - 1].len};
	}
	qsort(d->dead, d->ndead, sizeof(int64), sift_cmp_i64);
	qsort(docs, d->nd, sizeof(DocRef), cmp_doc);
	d->dids = malloc((d->nd + 1) * sizeof(int64));
	d->dpks = malloc((d->nd + 1) * sizeof(int64));
	d->text = malloc((d->nd + 1) * sizeof(char *));
	d->tlen = malloc((d->nd + 1) * sizeof(size_t));
	for (size_t i = 0; i < d->nd; i++)
	{
		d->dids[i] = docs[i].did;
		d->dpks[i] = docs[i].pk;
		d->text[i] = docs[i].text;
		d->tlen[i] = docs[i].len;
	}
	free(docs);
}

/*
 * The live segment over the queue rows ev: the text of the rows comes from the host, the segments' state from the
 * index. NULL when the index moved meanwhile (the caller reads the queue again).
 */
static Delta *
delta_make(SiftIdx *ix, const SxHost *h, const SxEvent *ev, size_t n, int64 after, uint64 epoch, uint64 sig)
{
	LastRow	   *g;
	size_t		ng = last_rows(ev, n, &g),
				nc = 0;
	int64	   *cand = malloc((ng + 1) * sizeof(int64));
	SxRow	   *rows = calloc(ng + 1, sizeof(SxRow));
	char		err[512];
	Delta	   *d = calloc(1, sizeof(Delta));

	for (size_t i = 0; i < ng; i++)
		if (g[i].op == 'I')
			cand[nc++] = g[i].pk;
	if (nc > DELTA_MAX_DOCS)
		fail("sift: %zu rows are waiting in the queue: run sift_sync", nc);
	if (nc && h->fetch(h->ctx, ix, cand, nc, rows, err, sizeof(err)) != 0)
		fail("%s", err);
	RD_LOCK(ix);
	if (ix->man.drained != after || ix->epoch != epoch)
	{
		RD_UNLOCK(ix);
		for (size_t i = 0; i < nc; i++)
			free(rows[i].text);
		free(g);
		free(cand);
		free(rows);
		free(d);
		return NULL;
	}
	d->refs = 2;
	d->epoch = epoch;
	d->after = after;
	d->sig = sig;
	d->nev = n;
	d->g = -1;
	delta_fill(ix, d, g, ng, nc, rows);
	RD_UNLOCK(ix);
	if (d->nd)
		delta_segment(ix, d);
	free(g);
	free(cand);
	free(rows);
	pthread_mutex_lock(&ix->dmu);
	delta_release(ix->delta);
	ix->delta = d;
	pthread_mutex_unlock(&ix->dmu);
	return d;
}

/* The live segment for the queue as it is now (NULL when the queue holds nothing the segments lack) */
static void
delta_acquire(Qry *q)
{
	SiftIdx    *ix = q->ix;
	const SxHost *h = q->h;
	char		err[512];

	for (int tries = 0; tries < RETRIES; tries++)
	{
		SxEvent    *ev;
		size_t		n;
		int64		after;
		uint64		epoch,
					sig;
		Delta	   *d;

		RD_LOCK(ix);
		after = ix->man.drained;
		epoch = ix->epoch;
		RD_UNLOCK(ix);
		if (h->queue(h->ctx, ix, after, &ev, &n, err, sizeof(err)) != 0)
			fail("%s", err);
		if (n == 0)
		{
			free(ev);
			return;
		}
		sig = events_sig(ev, n);
		d = delta_cached(ix, sig, after, epoch);
		if (d == NULL)
			d = delta_make(ix, h, ev, n, after, epoch, sig);
		free(ev);
		if (d)
		{
			q->dl = d;
			return;
		}
	}
	fail("sift: index \"%s\" kept changing while the query read its queue", ix->name);
}

/* ------------------------------------------------ the view of a query */

/* A document that does not count: superseded or deleted, retired by a queue row, or outside the filter */
static bool
excluded(void *ctx, int64 docid)
{
	Qry	   *q = ctx;

	if (is_dead(q->ix, docid))
		return true;
	if (q->dl && in_sorted(q->dl->dead, q->dl->ndead, docid))
		return true;
	return q->flt && !in_sorted(q->fdocs, q->nfdocs, docid);
}

static int64
live_pk(Qry *q, int64 docid)
{
	if (docid > q->dlfrom)
	{
		int64	   *at = bsearch(&docid, q->dl->dids, q->dl->nd, sizeof(int64), sift_cmp_i64);

		if (at == NULL)
			fail("sift: document %lld is not in the live segment", (long long) docid);
		return q->dl->dpks[at - q->dl->dids];
	}
	return pk_of(q->ix, docid);
}

static void
build_filter(Qry *q)
{
	SiftIdx    *ix = q->ix;
	const SxIdSet *f = q->flt;
	int64	   *docs = malloc((f->n + (q->dl ? q->dl->nd : 0) + 1) * sizeof(int64));
	size_t		nd = 0;

	for (size_t i = 0; i < f->n; i++)
	{
		int64		doc = latest_get(ix, f->ids[i]);

		if (doc)
			docs[nd++] = doc;
	}
	for (size_t i = 0; q->dl && i < q->dl->nd; i++)
		if (in_sorted(f->ids, f->n, q->dl->dpks[i]))
			docs[nd++] = q->dl->dids[i];
	qsort(docs, nd, sizeof(int64), sift_cmp_i64);
	q->fdocs = docs;
	q->nfdocs = nd;
	q->filter = sift_filter_from_docids(q->sr, docs, nd, CurrentMemoryContext);
}

static void
view_open(Qry *q)
{
	SiftIdx    *ix = q->ix;
	SiftAnalyzerConfig cfg;
	int			n = ix->man.nsegs;

	q->rel = sift_rel_view(ix->sr, &q->sr);
	sift_analyzer_config_from_options(&ix->sr->opts, &cfg);
	memset(&q->snap, 0, sizeof(q->snap));
	q->snap.meta.analyzer_hash = cfg.hash;
	q->snap.meta.k1 = ix->sr->opts.k1;
	q->snap.meta.b = ix->sr->opts.b;
	for (int i = 0; i < n; i++)
		q->snap.segs[i] = ix->segs[i].e;
	if (q->dl && q->dl->g >= 0)
	{
		q->snap.segs[n++] = q->dl->e;
		sift_slot_attach(q->sr, SIFT_DELTA_SLOT, q->dl->g, q->dl->blocks);
	}
	q->snap.nsegs = n;
	q->dlfrom = ix->man.drained + ix->man.docbase;
	q->sr->is_dead = excluded;
	q->sr->dead_ctx = q;
	q->sr->any_dead = ix->ndead > 0 || (q->dl && q->dl->ndead > 0) || q->flt;
	if (q->flt)
		build_filter(q);
}

static void
view_close(Qry *q)
{
	if (q->rel)
		sift_rel_view_free(q->rel);
	free(q->fdocs);
	q->rel = NULL;
	q->fdocs = NULL;
}

static SiftIndexState *
open_state(Qry *q, MemoryContext cx)
{
	return sift_state_open_dir(q->rel, cx, &q->snap);
}

static SiftPlan *
bind_query(Qry *q, SiftIndexState *st, int flags)
{
	SiftQuery **query = palloc(sizeof(SiftQuery *));	/* the plan keeps the array */

	*query = sift_state_query(st, q->q, q->qlen);
	return sift_plan_bind(st, query, 1, flags);
}

/* The text of a document for the recheck of a phrase: the live segment keeps it, the others ask the host for the row */
static bool
load_doc(Qry *q, int64 docid, text **doc)
{
	const char *t;
	size_t		len;
	SxRow		row = {0};
	int64		pk;
	char		err[512];

	if (docid > q->dlfrom)
	{
		int64	   *at = bsearch(&docid, q->dl->dids, q->dl->nd, sizeof(int64), sift_cmp_i64);

		t = q->dl->text[at - q->dl->dids];
		len = q->dl->tlen[at - q->dl->dids];
	}
	else
	{
		if (q->h == NULL)
			fail("sift: this phrase needs the text of the row, which a call without the table cannot read");
		pk = pk_of(q->ix, docid);
		if (q->h->fetch(q->h->ctx, q->ix, &pk, 1, &row, err, sizeof(err)) != 0)
			fail("%s", err);
		if (row.text == NULL)
			return false;
		t = row.text;
		len = row.len;
	}
	*doc = palloc(VARHDRSZ + len);
	SET_VARSIZE(*doc, VARHDRSZ + len);
	memcpy(VARDATA_ANY(*doc), t, len);
	free(row.text);
	return true;
}

static bool
verify_cb(void *arg, ItemPointer tid, bool want_doc, text **doc)
{
	Qry	   *q = arg;
	int64		docid = sift_key_docid(q->sr, sift_key_from_tid(tid));

	if (excluded(q, docid))
		return false;
	return !want_doc || load_doc(q, docid, doc);
}

/* ------------------------------------------------ top-k, count, matches */

/* Keeps the candidates whose row still exists, in order, appending them to the result */
static void
keep_existing(Qry *q, const SxHit *cand, int n)
{
	int64	   *pks = palloc(n * sizeof(int64));
	uint8	   *present = palloc(n);
	char		err[512];

	if (n == 0)
		return;
	for (int i = 0; i < n; i++)
		pks[i] = cand[i].pk;
	memset(present, 1, n);
	if (q->h && q->ix->recheck && q->h->exists(q->h->ctx, q->ix, pks, n, present, err, sizeof(err)) != 0)
		fail("%s", err);
	for (int i = 0; i < n; i++)
		if (present[i])
			q->hits[q->n++] = cand[i];
}

static void
run_topk(Qry *q, MemoryContext cx)
{
	SiftIndexState *st = open_state(q, cx);
	SiftPlan   *plan = bind_query(q, st, SIFT_BIND_SCORE);
	SiftRanker *r;
	ItemPointerData tid;
	float4		score;

	if (q->filter)
		sift_plan_set_filter(plan, q->filter);
	r = sift_rank_begin(plan, q->k, verify_cb, q);
	while (q->n < q->k)
	{
		int			want = q->k - q->n,
					got = 0;
		SxHit	   *cand = palloc(want * sizeof(SxHit));

		while (got < want && sift_rank_next(r, &tid, &score))
			cand[got++] = (SxHit) {live_pk(q, sift_key_docid(q->sr, sift_key_from_tid(&tid))), score};
		keep_existing(q, cand, got);
		if (got < want)
			break;
	}
	sift_rank_end(r);
	sift_state_close(st);
}

static void
run_count(Qry *q, MemoryContext cx)
{
	SiftIndexState *st = open_state(q, cx);
	SiftPlan   *plan = bind_query(q, st, 0);
	SiftVis		vis = {.heap = q->rel};

	q->count = sift_eval_count(plan, &vis, verify_cb, q);
	sift_state_close(st);
}

static void
collect_page(void *arg, BlockNumber blk, const uint64 *offmask)
{
	Qry	   *q = arg;

	for (int w = 0; w < SIFT_OFFMASK_WORDS; w++)
		for (uint64 m = offmask[w]; m; m &= m - 1)
		{
			int64		docid = (int64) blk * q->sr->dpp + w * 64 + __builtin_ctzll(m);

			if (q->npks == q->cappks)
				q->pks = realloc(q->pks, (q->cappks = q->cappks ? q->cappks * 2 : 1024) * sizeof(int64));
			q->pks[q->npks++] = live_pk(q, docid);
		}
}

static void
run_matches(Qry *q, MemoryContext cx)
{
	SiftIndexState *st = open_state(q, cx);
	SiftPlan   *plan = bind_query(q, st, 0);
	SiftVis		vis = {.heap = q->rel};

	sift_eval_pages(plan, &vis, verify_cb, q, collect_page, q);
	sift_state_close(st);
}

/* ------------------------------------------------ public */

static void
run_body(Qry *q, void (*body) (Qry *, MemoryContext), MemoryContext cx)
{
	view_open(q);
	body(q, cx);
}

static int
run_query(Qry *q, void (*body) (Qry *, MemoryContext), char *err, size_t errlen)
{
	SiftIdx    *ix = q->ix;
	MemoryContext cx;
	int			rc = -1;

	thread_init();
	cx = op_begin();
	MemoryContextSwitchTo(cx);
	for (int tries = 0; tries < RETRIES && rc != 0; tries++)
	{
		bool		again = false;

		q->dl = NULL;
		if (q->h && ix->fresh)
			rc = SIFT_TRY(delta_acquire(q));
		else
			rc = 0;
		if (rc != 0)
			break;
		RD_LOCK(ix);
		if (ix->dropped)
		{
			snprintf(sift_err_msg, sizeof(sift_err_msg), "sift: index \"%s\" was dropped", ix->name);
			rc = -1;
		}
		else if (q->dl && (ix->man.drained != q->dl->after || ix->epoch != q->dl->epoch))
			again = true;
		else
			rc = SIFT_TRY(run_body(q, body, cx));
		view_close(q);
		RD_UNLOCK(ix);
		delta_release(q->dl);
		q->dl = NULL;
		if (!again)
			break;
		rc = -1;
		snprintf(sift_err_msg, sizeof(sift_err_msg), "sift: index \"%s\" kept changing while the query ran", ix->name);
	}
	op_end(cx);
	return finish_call(rc, err, errlen);
}

int
sx_topk(SiftIdx *ix, const SxHost *h, const char *qs, int qlen, int k, const SxIdSet *flt, SxHit *hits, int *n, char *err,
		size_t errlen)
{
	Qry		q = {.ix = ix,.h = h,.q = qs,.qlen = qlen,.k = k,.flt = flt,.hits = hits};
	int			rc = run_query(&q, run_topk, err, errlen);

	*n = q.n;
	return rc;
}

int
sx_count(SiftIdx *ix, const SxHost *h, const char *qs, int qlen, const SxIdSet *flt, int64_t *count, char *err,
		 size_t errlen)
{
	Qry		q = {.ix = ix,.h = h,.q = qs,.qlen = qlen,.flt = flt};
	int			rc = run_query(&q, run_count, err, errlen);

	*count = q.count;
	return rc;
}

int
sx_matches(SiftIdx *ix, const SxHost *h, const char *qs, int qlen, const SxIdSet *flt, int64_t **pks, int64_t *n, char *err,
		   size_t errlen)
{
	Qry		q = {.ix = ix,.h = h,.q = qs,.qlen = qlen,.flt = flt};
	int			rc = run_query(&q, run_matches, err, errlen);

	*pks = q.pks;
	*n = q.npks;
	return rc;
}
