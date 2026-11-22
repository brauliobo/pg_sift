/*
 * delta.c - the live segment: the queue rows the segments do not hold yet, as in-memory segments of their own.
 *
 * A query asks the host how many queue rows wait above the watermark and the highest seq among them (one row back): the
 * cached live segment answers when both are what it was built over, whatever else the queue did. When rows were added, the
 * live segment grows by a micro segment instead of being built again: the first one (the primary, slot SIFT_DELTA_SLOT) stays
 * as it is, and a second segment (slot SIFT_DELTA2_SLOT) holds the documents of the rows that came after it, built again from
 * the few documents it holds plus the new ones. It is merged into a new primary when it outgrows sqrt(documents of the
 * primary). A row that is not the next one in seq (a commit that came late) cannot be told from the rows read, so the live
 * segment is built again from the whole queue.
 * The dead list of a micro segment is the whole dead list of the queue (segment documents and documents of the primary a
 * row retires), so the query reads only that one.
 */
#include "ix.h"

#include <math.h>
#include <unistd.h>

#include "bm25/analyze.h"
#include "bm25/bitmap.h"
#include "core/cmp.h"
#include "bm25/eval.h"
#include "bm25/kernels.h"
#include "bm25/runs.h"

#define DELTA_BUDGET	((Size) 256 << 20)
#define DELTA_MAX_DOCS	500000
#define MICRO_MIN		64			/* documents a micro segment may hold whatever the primary */
#define PKMAP_MIN		64			/* documents from which a segment keeps a table of its keys */
#define RETRIES			8

typedef struct LastRow				/* the last queue row of a key */
{
	int64		pk;
	int64		seq;
	char		op;
} LastRow;

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
	LastRow	   *g = malloc((n + 1) * sizeof(LastRow));
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

/* ------------------------------------------------ documents of a live segment */

/* The documents of d are its own, ascending, after those of its root (a micro segment's) */
bool
delta_doc(const Delta *d, int64 docid, const Delta **owner, size_t *idx)
{
	for (; d; d = d->root)
	{
		size_t		i = lower_bound(d->dids, d->nd, docid);

		if (i < d->nd && d->dids[i] == docid)
		{
			*owner = d;
			*idx = i;
			return true;
		}
	}
	return false;
}

size_t
delta_ndocs(const Delta *d)
{
	return d->nd + (d->root ? d->root->nd : 0);
}

/* The live document the segment holds of key pk (its own, else its root's), 0 when there is none */
int64
delta_pk_doc(const Delta *head, int64 pk)
{
	for (const Delta *d = head; d; d = d->root)
	{
		int64		doc = 0;

		if (d->pk_map)
			doc = slot_find(d->pk_map, d->pk_cap, pk)->val;
		else
			for (size_t i = 0; i < d->nd && !doc; i++)
				doc = d->dpks[i] == pk ? d->dids[i] : 0;
		if (doc)
			return in_sorted(head->dead, head->ndead, doc) ? 0 : doc;
	}
	return 0;
}

static void
pk_map_build(Delta *d)
{
	if (d->nd < PKMAP_MIN)
		return;
	d->pk_cap = 16;
	while (d->pk_cap < d->nd * 2)
		d->pk_cap *= 2;
	d->pk_map = calloc(d->pk_cap, sizeof(Slot));
	for (size_t i = 0; i < d->nd; i++)
		*slot_find(d->pk_map, d->pk_cap, d->dpks[i]) = (Slot) {d->dpks[i], d->dids[i], 1};
}

void
delta_release(Delta *d)
{
	if (d == NULL || __atomic_sub_fetch(&d->refs, 1, __ATOMIC_ACQ_REL) > 0)
		return;
	if (d->g >= 0)
		sift_unmap(d->g, d->blocks);
	for (size_t i = 0; i < d->nd; i++)
		free(d->text[i]);
	delta_release(d->root);
	free(d->dead);
	free(d->dids);
	free(d->dpks);
	free(d->text);
	free(d->tlen);
	free(d->pk_map);
	free(d);
}

/* Forgets the cached live segment (a writer published: the queue rows it was built over are in the segments) */
void
delta_drop(SiftIdx *ix)
{
	Delta	   *d;

	pthread_mutex_lock(&ix->dmu);
	d = ix->delta;
	ix->delta = NULL;
	pthread_mutex_unlock(&ix->dmu);
	delta_release(d);
}

/* ------------------------------------------------ building */

/* Analyzes the documents of d and maps them as a segment in memory at slot */
static void
delta_segment(SiftIdx *ix, Delta *d, int slot)
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
	wt = sift_wt_begin_at(ix->sr, slot, true);
	sift_inv_write(inv, wt, ((uint64) 0xFFFF << 48) | __atomic_add_fetch(&counter, 1, __ATOMIC_RELAXED), &ref, &d->e);
	sift_wt_finish(wt);
	d->g = sift_map_fd(sift_wt_fd(wt), slot, &d->blocks);
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

/* The ascending docids of a and b without repeats */
static size_t
union_ids(const int64 *a, size_t na, const int64 *b, size_t nb, int64 **out)
{
	int64	   *u = malloc((na + nb + 1) * sizeof(int64));
	size_t		i = 0,
				j = 0,
				n = 0;

	while (i < na || j < nb)
	{
		int64		v = j == nb || (i < na && a[i] <= b[j]) ? a[i] : b[j];

		u[n++] = v;
		while (i < na && a[i] == v)
			i++;
		while (j < nb && b[j] == v)
			j++;
	}
	*out = u;
	return n;
}

/*
 * The documents of d: those the queue rows g add (rows[i] is the text of g[i]) and, for a micro segment, the live ones of prev; and
 * its dead list: what the rows retire (the live document of the key in the segments, in prev) and, for a micro segment, prev's.
 */
static void
delta_fill(SiftIdx *ix, Delta *d, const Delta *prev, const LastRow *g, size_t ng, const SxRow *rows)
{
	int64	   *ret = malloc((ng * 2 + 1) * sizeof(int64));
	size_t		nret = 0;
	DocRef	   *docs;
	size_t		nprev = prev && prev->root ? prev->nd : 0;	/* the documents a micro segment holds already */

	for (size_t i = 0; i < ng; i++)
	{
		int64		doc = latest_get(ix, g[i].pk),
					in_prev = prev ? delta_pk_doc(prev, g[i].pk) : 0;

		if (doc)
			ret[nret++] = doc;
		if (in_prev)
			ret[nret++] = in_prev;
	}
	qsort(ret, nret, sizeof(int64), sift_cmp_i64);
	d->ndead = union_ids(ret, nret, prev ? prev->dead : NULL, prev ? prev->ndead : 0, &d->dead);
	free(ret);
	docs = malloc((ng + nprev + 1) * sizeof(DocRef));
	for (size_t i = 0; i < nprev; i++)
		if (!in_sorted(d->dead, d->ndead, prev->dids[i]))
		{
			size_t		len = prev->tlen[i];
			char	   *t = malloc(len + 1);

			memcpy(t, prev->text[i], len);
			docs[d->nd++] = (DocRef) {prev->dids[i], prev->dpks[i], t, len};
		}
	for (size_t i = 0; i < ng; i++)
		if (g[i].op == 'I' && rows[i].text)
			docs[d->nd++] = (DocRef) {g[i].seq + ix->man.docbase, g[i].pk, rows[i].text, rows[i].len};
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
	pk_map_build(d);
}

/* The text of each 'I' row of g: that of the document prev already holds of it, else the host's */
static void
delta_texts(SiftIdx *ix, const SxHost *h, const Delta *reuse, const LastRow *g, size_t ng, SxRow *rows)
{
	int64	   *cand = malloc((ng + 1) * sizeof(int64));
	size_t	   *at = malloc((ng + 1) * sizeof(size_t));
	SxRow	   *got = calloc(ng + 1, sizeof(SxRow));
	size_t		nc = 0;
	char		err[512];

	for (size_t i = 0; i < ng; i++)
	{
		const Delta *own;
		size_t		k;

		if (g[i].op != 'I')
			continue;
		if (reuse && delta_doc(reuse, g[i].seq + ix->man.docbase, &own, &k))
		{
			rows[i] = (SxRow) {g[i].pk, malloc(own->tlen[k] + 1), own->tlen[k]};
			memcpy(rows[i].text, own->text[k], own->tlen[k]);
			continue;
		}
		at[nc] = i;
		cand[nc++] = g[i].pk;
	}
	if (nc > DELTA_MAX_DOCS)
		fail("sift: %zu rows are waiting in the queue: run sift_sync", nc);
	if (nc && h->fetch(h->ctx, ix, cand, nc, got, err, sizeof(err)) != 0)
		fail("%s", err);
	for (size_t j = 0; j < nc; j++)
		rows[at[j]] = got[j];
	free(cand);
	free(at);
	free(got);
}

/*
 * The live segment over the queue rows ev (all above `after`, or with prev, those above prev's): a micro segment over prev's
 * primary when micro, else a primary. NULL when the index moved meanwhile (*moved), the caller reads the queue again.
 */
static Delta *
delta_make(SiftIdx *ix, const SxHost *h, const SxEvent *ev, size_t n, int64 after, uint64 epoch, const Delta *prev, bool micro,
		   bool *moved)
{
	LastRow	   *g;
	size_t		ng = last_rows(ev, n, &g);
	SxRow	   *rows = calloc(ng + 1, sizeof(SxRow));
	Delta	   *d = calloc(1, sizeof(Delta));

	delta_texts(ix, h, prev, g, ng, rows);
	RD_LOCK(ix);
	if (ix->man.drained != after || ix->epoch != epoch)
	{
		RD_UNLOCK(ix);
		for (size_t i = 0; i < ng; i++)
			free(rows[i].text);
		*moved = true;
		free(g);
		free(rows);
		free(d);
		return NULL;
	}
	d->refs = 2;
	d->epoch = epoch;
	d->after = after;
	d->max_seq = ev[n - 1].seq;
	d->nev = (micro ? prev->nev : 0) + n;
	d->g = -1;
	if (micro)
	{
		d->root = prev->root ? prev->root : (Delta *) prev;
		__atomic_add_fetch(&d->root->refs, 1, __ATOMIC_ACQ_REL);
	}
	delta_fill(ix, d, micro ? prev : NULL, g, ng, rows);
	RD_UNLOCK(ix);
	if (d->nd)
		delta_segment(ix, d, micro ? SIFT_DELTA2_SLOT : SIFT_DELTA_SLOT);
	free(g);
	free(rows);
	pthread_mutex_lock(&ix->dmu);
	delta_release(ix->delta);
	ix->delta = d;
	pthread_mutex_unlock(&ix->dmu);
	return d;
}

/* Rows above `after`, all of them, or only those above prev's last when prev is given */
static size_t
queue_rows(SiftIdx *ix, const SxHost *h, int64 after, SxEvent **ev)
{
	char		err[512];
	size_t		n;

	if (h->queue(h->ctx, ix, after, ev, &n, err, sizeof(err)) != 0)
		fail("%s", err);
	return n;
}

/*
 * The cached live segment grown by the rows after it, as a micro segment over its primary; NULL when it cannot: the queue is not
 * prev's rows and exactly count - prev->nev rows after it (a row committed late is not among them), or the micro segment would
 * outgrow its bound
 */
static Delta *
delta_extend(SiftIdx *ix, const SxHost *h, const Delta *prev, int64 after, uint64 epoch, size_t count, bool *moved)
{
	const Delta *root = prev->root ? prev->root : prev;
	SxEvent    *ev;
	size_t		n = queue_rows(ix, h, prev->max_seq, &ev),
				nins = 0;
	Delta	   *d = NULL;

	for (size_t i = 0; i < n; i++)
		nins += ev[i].op == 'I';
	if (root->nd > 0 && n > 0 && n == count - prev->nev &&
		(prev->root ? prev->nd : 0) + nins <= Max(MICRO_MIN, (size_t) sqrt((double) root->nd)))
		d = delta_make(ix, h, ev, n, after, epoch, prev, true, moved);
	free(ev);
	return d;
}

/* The cached live segment if it was built over this after and epoch (a reference is taken) */
static Delta *
delta_cached(SiftIdx *ix, int64 after, uint64 epoch)
{
	Delta	   *d;

	pthread_mutex_lock(&ix->dmu);
	d = ix->delta;
	if (d && d->after == after && d->epoch == epoch)
		__atomic_add_fetch(&d->refs, 1, __ATOMIC_ACQ_REL);
	else
		d = NULL;
	pthread_mutex_unlock(&ix->dmu);
	return d;
}

/* A primary over the whole queue (prev's texts are reused); NULL when the queue holds nothing the segments lack */
static Delta *
delta_rebuild(SiftIdx *ix, const SxHost *h, const Delta *prev, int64 after, uint64 epoch, bool *moved)
{
	SxEvent    *ev;
	size_t		n = queue_rows(ix, h, after, &ev);
	Delta	   *d = NULL;

	if (n > 0)
		d = delta_make(ix, h, ev, n, after, epoch, prev, false, moved);
	free(ev);
	return d;
}

/* The live segment for the queue as it is now (NULL when the queue holds nothing the segments lack); a reference is returned */
Delta *
delta_acquire(SiftIdx *ix, const SxHost *h)
{
	char		err[512];

	for (int tries = 0; tries < RETRIES; tries++)
	{
		int64		after,
					max_seq;
		uint64		epoch;
		size_t		count;
		Delta	   *prev,
				   *d = NULL;
		bool		moved = false;

		RD_LOCK(ix);
		after = ix->man.drained;
		epoch = ix->epoch;
		RD_UNLOCK(ix);
		if (h->queue_probe(h->ctx, ix, after, &max_seq, &count, err, sizeof(err)) != 0)
			fail("%s", err);
		if (count == 0)
			return NULL;
		prev = delta_cached(ix, after, epoch);
		if (prev && prev->nev == count && prev->max_seq == max_seq)
			return prev;
		if (prev && prev->max_seq < max_seq && prev->nev < count)
			d = delta_extend(ix, h, prev, after, epoch, count, &moved);
		if (d == NULL && !moved)
			d = delta_rebuild(ix, h, prev, after, epoch, &moved);
		delta_release(prev);
		if (d || !moved)
			return d;
	}
	fail("sift: index \"%s\" kept changing while the query read its queue", ix->name);
}
