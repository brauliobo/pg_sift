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

#include "bm25/analyze.h"
#include "bm25/bitmap.h"
#include "core/cmp.h"
#include "bm25/eval.h"
#include "bm25/kernels.h"
#include "bm25/runs.h"

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
	SiftDirSnapshot *snap;
	Delta	   *dl;
	int64		dlfrom;				/* docids above belong to the live segment */
	int64	   *fdocs;				/* documents the filter allows, ascending */
	size_t		nfdocs;
	SiftFilter *filter;
	size_t		id_pos,				/* where live_pk_seq found the last document in the keys of the index, in the live segment and in its primary */
				dl_pos,
				root_pos;
} Qry;

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
		const Delta *own;
		size_t		i;

		if (!delta_doc(q->dl, docid, &own, &i))
			fail("sift: document %lld is not in the live segment", (long long) docid);
		return own->dpks[i];
	}
	return pk_of(q->ix, docid);
}

static void
build_filter(Qry *q)
{
	SiftIdx    *ix = q->ix;
	const SxIdSet *f = q->flt;
	size_t		ndelta = q->dl ? delta_ndocs(q->dl) : 0;
	int64	   *docs = xmalloc((f->n + ndelta + 1) * sizeof(int64));
	size_t		nd = 0;

	for (size_t i = 0; i < f->n; i++)
	{
		int64		doc = latest_get(ix, f->ids[i]);

		if (doc)
			docs[nd++] = doc;
	}
	if (q->dl && f->n * 8 < ndelta)		/* the keys of the filter are fewer: look each up in the live segment */
		for (size_t i = 0; i < f->n; i++)
		{
			int64		doc = delta_pk_doc(q->dl, f->ids[i]);

			if (doc)
				docs[nd++] = doc;
		}
	else
		for (const Delta *d = q->dl; d; d = d->root)
			for (size_t i = 0; i < d->nd; i++)
				if (in_sorted(f->ids, f->n, d->dpks[i]) && !in_sorted(q->dl->dead, q->dl->ndead, d->dids[i]))
					docs[nd++] = d->dids[i];
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
	q->snap = sift_dir_alloc(n + 2);
	q->snap->meta.analyzer_hash = cfg.hash;
	q->snap->meta.k1 = ix->sr->opts.k1;
	q->snap->meta.b = ix->sr->opts.b;
	for (int i = 0; i < n; i++)
		q->snap->segs[i] = ix->segs[i].e;
	for (const Delta *d = q->dl; d; d = d->root)
		if (d->g >= 0)
		{
			q->snap->segs[n++] = d->e;
			sift_slot_attach(q->sr, d->root ? SIFT_DELTA2_SLOT : SIFT_DELTA_SLOT, d->g, d->blocks);
		}
	q->snap->nsegs = n;
	q->dlfrom = ix->man.drained + ix->man.docbase;
	q->sr->dead[0] = ix->dead;
	q->sr->ndead[0] = ix->ndead;
	if (q->dl)
	{
		q->sr->dead[1] = q->dl->dead;
		q->sr->ndead[1] = q->dl->ndead;
	}
	if (q->flt)
		build_filter(q);
}

static void
view_close(Qry *q)
{
	if (q->rel)
		sift_rel_view_free(q->rel);
	free(q->fdocs);
	free(q->snap);
	q->rel = NULL;
	q->fdocs = NULL;
	q->snap = NULL;
}

static SiftIndexState *
open_state(Qry *q, MemoryContext cx)
{
	return sift_state_open_dir(q->rel, cx, q->snap);
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
		const Delta *own;
		size_t		i;

		if (!delta_doc(q->dl, docid, &own, &i))
			fail("sift: document %lld is not in the live segment", (long long) docid);
		t = own->text[i];
		len = own->tlen[i];
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

	if (q->filter)
		sift_plan_set_filter(plan, q->filter);
	q->count = sift_eval_count(plan, &vis, verify_cb, q);
	sift_state_close(st);
}

/* live_pk for documents in ascending order, as sift_eval_pages gives them: each search starts where the last one ended */
static int64
live_pk_seq(Qry *q, int64 docid)
{
	if (docid > q->dlfrom)
	{
		const Delta *d = q->dl;
		size_t	   *pos = &q->dl_pos;

		if (d->root && (d->nd == 0 || docid < d->dids[0]))		/* the primary's documents come before the micro segment's */
		{
			d = d->root;
			pos = &q->root_pos;
		}
		*pos = lower_bound_from(d->dids, d->nd, *pos, docid);
		if (*pos == d->nd || d->dids[*pos] != docid)
			fail("sift: document %lld is not in the live segment", (long long) docid);
		return d->dpks[*pos];
	}
	q->id_pos = lower_bound_from(q->ix->ids, q->ix->ndoc, q->id_pos, docid);
	if (q->id_pos == q->ix->ndoc || q->ix->ids[q->id_pos] != docid)
		fail("sift: document %lld has no row key in the event log", (long long) docid);
	return q->ix->pks[q->id_pos];
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
				q->pks = xrealloc(q->pks, (q->cappks = q->cappks ? q->cappks * 2 : 1024) * sizeof(int64));
			q->pks[q->npks++] = live_pk_seq(q, docid);
		}
}

static void
run_matches(Qry *q, MemoryContext cx)
{
	SiftIndexState *st = open_state(q, cx);
	SiftPlan   *plan = bind_query(q, st, 0);
	SiftVis		vis = {.heap = q->rel};

	if (q->filter)
		sift_plan_set_filter(plan, q->filter);
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
			rc = SIFT_TRY(q->dl = delta_acquire(ix, q->h));
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
