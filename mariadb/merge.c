/*
 * merge.c - tiered merge of segments and the background thread that runs it.
 *
 * Policy. A segment belongs to a tier by size (factor 4 from 1 MiB); when `merge_fanin` segments of a tier have piled up
 * (every drain adds a small one) they are merged into one of the next tier, so a document is rewritten once per tier and
 * the number of segments stays logarithmic in the size of the index. An index whose dead documents exceed a quarter of
 * its documents, or that comes near the segment limit, is merged more widely; sx_merge(full) merges everything.
 * Streaming. Each source segment becomes a sorted run in a temporary file and the runs are merged page by page: memory
 * does not depend on the size of the segments, only on how many are merged at once (at most 4 * fanin).
 * Concurrency. The sources are read through a private mapping of their files (the merge clears liveness bits of the
 * documents it drops in its copy only), so queries on the index go on; the merge holds no lock but `mergemu` until it
 * publishes: the manifest and log are written, then the segment table, the document tables and the slots are swapped in
 * under the write lock and the old files are unlinked. A crash before the manifest rename leaves the old state (the new
 * file is removed at open), after it the new state (the old files are removed at open).
 */
#include "postgres.h"

#include <unistd.h>

#include "utils/memutils.h"

#include "sift_build.h"
#include "ix.h"

#define MAX_SOURCES(ix)		((ix)->fanin * 4)

static pthread_t bg_thread;
static pthread_mutex_t bg_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t bg_cv = PTHREAD_COND_INITIALIZER;
static bool bg_started,
			bg_stop,
			bg_kicked;

typedef struct Plan
{
	int			n;
	ManSeg		src[SIFT_MAX_SEGMENTS];
} Plan;

typedef struct Dropped
{
	const int64 *dead;			/* the dead documents when the merge began, ascending */
	size_t		ndead;
	int64	   *docs;			/* those it dropped, ascending */
	size_t		n,
				cap;
} Dropped;

static int
tier_of(int64 bytes)
{
	int			t = 0;

	for (int64 mb = bytes >> 20; mb >= 1; mb >>= 2)
		t++;
	return t;
}

/* The segments to merge now, copied from the manifest; plan->n = 0 when there is nothing to do */
static void
pick(SiftIdx *ix, bool full, Plan *plan)
{
	int			count[32] = {0};
	int			nsegs;
	bool		wide;

	plan->n = 0;
	RD_LOCK(ix);
	nsegs = ix->man.nsegs;
	wide = (ix->ndead > 64 && ix->ndead * 4 > ix->ndoc) || nsegs > MAX_SEGS * 3 / 4;
	if (full || wide)
	{
		if (nsegs > 1 || (nsegs == 1 && ix->ndead > 0))
		{
			memcpy(plan->src, ix->segs, nsegs * sizeof(ManSeg));
			plan->n = nsegs;
		}
		RD_UNLOCK(ix);
		return;
	}
	for (int i = 0; i < nsegs; i++)
		count[Min(tier_of(ix->segs[i].e.bytes), 31)]++;
	for (int t = 0; t < 32; t++)
		if (count[t] >= ix->fanin)
		{
			for (int i = 0; i < nsegs && plan->n < MAX_SOURCES(ix); i++)
				if (Min(tier_of(ix->segs[i].e.bytes), 31) == t)
					plan->src[plan->n++] = ix->segs[i];
			break;
		}
	RD_UNLOCK(ix);
}

typedef struct MergeCtx
{
	Dropped		d;
	const SiftRel *sr;
} MergeCtx;

static bool
merge_dead_cb(ItemPointer tid, void *state)
{
	MergeCtx   *m = state;
	int64		docid = sift_key_docid(m->sr, sift_key_from_tid(tid));
	size_t		lo = 0,
				hi = m->d.ndead;

	while (lo < hi)
	{
		size_t		mid = (lo + hi) / 2;

		if (m->d.dead[mid] < docid)
			lo = mid + 1;
		else
			hi = mid;
	}
	if (lo == m->d.ndead || m->d.dead[lo] != docid)
		return false;
	if (m->d.n == m->d.cap)
		m->d.docs = realloc(m->d.docs, (m->d.cap = m->d.cap ? m->d.cap * 2 : 1024) * sizeof(int64));
	m->d.docs[m->d.n++] = docid;
	return true;
}

static int
cmp_i64(const void *a, const void *b)
{
	int64		x = *(const int64 *) a,
				y = *(const int64 *) b;

	return (x > y) - (x < y);
}

typedef struct Merged
{
	ManSeg		ms;
	bool		have;
	int			g;
	uint32		blocks;
	int64	   *dropped;
	size_t		ndropped;
} Merged;

/* Merges the segments of the plan into one file, mapped but not part of the index yet */
static void
merge_write(SiftIdx *ix, const Plan *plan, Relation mrel, const int64 *dead, size_t ndead, Merged *out)
{
	MemoryContext cx = CurrentMemoryContext;
	SiftScoreRef ref = {ix->sr->opts.k1, ix->sr->opts.b, 0};
	SiftRun   **runs = palloc(plan->n * sizeof(SiftRun *));
	int			nruns = 0;
	SiftRel    *msr = mrel->rd_amcache;
	MergeCtx	mc = {.d = {.dead = dead,.ndead = ndead},.sr = msr};
	SiftWriteTarget *wt;
	uint64		gen;
	int			slot;

	for (int i = 0; i < plan->n; i++)
	{
		char		path[600];
		SiftSegment *seg;
		IndexBulkDeleteResult stats = {0};
		SiftRun    *r;

		path_of(ix, path, sizeof(path), plan->src[i].file);
		sift_slot_map(msr, plan->src[i].slot, path);
		seg = sift_seg_open(mrel, &plan->src[i].e, cx);
		if (ndead > 0)
			sift_seg_bulkdelete(seg, merge_dead_cb, &mc, &stats);
		if ((r = sift_run_from_segment(seg, &ref, cx)) != NULL)
			runs[nruns++] = r;
		sift_seg_close(seg);
	}
	out->have = nruns > 0;
	if (out->have)
	{
		gen = gen_take(ix);
		slot = slot_reserve(ix);
		wt = sift_wt_begin_at(ix->sr, slot, false);
		sift_runs_write(runs, nruns, wt, gen, &ref, &out->ms.e);
		sift_wt_finish(wt);
		rename_segment(ix, wt, gen, &out->ms, &out->g, &out->blocks);
	}
	qsort(mc.d.docs, mc.d.n, sizeof(int64), cmp_i64);
	out->dropped = mc.d.docs;
	out->ndropped = mc.d.n;
}

static bool
in_plan(const Plan *plan, const ManSeg *s)
{
	for (int i = 0; i < plan->n; i++)
		if (strcmp(plan->src[i].file, s->file) == 0)
			return true;
	return false;
}

/* The keys and dead documents that remain once the dropped ones are gone; returns the new count of documents */
static size_t
filter_docs(const SiftIdx *ix, const Merged *mg, int64 **ids, int64 **pks, int64 **dead, size_t *ndead)
{
	size_t		n = 0,
				nd = 0;

	*ids = malloc((ix->ndoc + 1) * sizeof(int64));
	*pks = malloc((ix->ndoc + 1) * sizeof(int64));
	*dead = malloc((ix->ndead + 1) * sizeof(int64));
	for (size_t i = 0; i < ix->ndoc; i++)
		if (!bsearch(&ix->ids[i], mg->dropped, mg->ndropped, sizeof(int64), cmp_i64))
		{
			(*ids)[n] = ix->ids[i];
			(*pks)[n++] = ix->pks[i];
		}
	for (size_t i = 0; i < ix->ndead; i++)
		if (!bsearch(&ix->dead[i], mg->dropped, mg->ndropped, sizeof(int64), cmp_i64))
			(*dead)[nd++] = ix->dead[i];
	*ndead = nd;
	return n;
}

static void
publish_merge(SiftIdx *ix, const Plan *plan, Merged *mg, int *oldg, uint32 *oldblocks)
{
	Manifest	m = ix->man;
	ManSeg		segs[SIFT_MAX_SEGMENTS];
	int64	   *ids,
			   *pks,
			   *dead;
	size_t		n,
				ndead;
	uint64		len;
	int			nkeep = 0;
	int64		bytes = 0;

	for (uint32 i = 0; i < m.nsegs; i++)
		if (!in_plan(plan, &ix->segs[i]))
			segs[nkeep++] = ix->segs[i];
	if (nkeep + plan->n != (int) m.nsegs)
		fail("sift: a segment of the merge is gone");
	if (mg->have)
		segs[nkeep++] = mg->ms;
	n = filter_docs(ix, mg, &ids, &pks, &dead, &ndead);
	m.nsegs = nkeep;
	m.logno++;
	m.version++;
	write_compact_log(ix, m.logno, ids, pks, n, &len);
	m.log_len = len;
	write_manifest(ix, &m, segs);
	WR_LOCK(ix);
	for (int i = 0; i < plan->n; i++)
	{
		oldg[i] = ix->sr->gslot[plan->src[i].slot];
		oldblocks[i] = ix->sr->slot_blocks[plan->src[i].slot];
		sift_slot_detach(ix->sr, plan->src[i].slot);
	}
	free(ix->ids);
	free(ix->pks);
	free(ix->dead);
	ix->ids = ids;
	ix->pks = pks;
	ix->dead = dead;
	ix->ndoc = n;
	ix->capdoc = ix->ndoc + 1;
	ix->ndead = ndead;
	ix->capdead = ndead + 1;
	ix->man.logno = m.logno;
	ix->man.log_len = m.log_len;
	ix->man.version = m.version;
	ix->man.nsegs = nkeep;
	memcpy(ix->segs, segs, nkeep * sizeof(ManSeg));
	if (mg->have)
	{
		sift_slot_attach(ix->sr, mg->ms.slot, mg->g, mg->blocks);
		ix->slot_reserved[mg->ms.slot] = false;
	}
	for (int i = 0; i < nkeep; i++)
		bytes += (int64) ix->sr->slot_blocks[segs[i].slot] * BLCKSZ;
	ix->bytes = bytes;
	ix->merges++;
	ix->sr->any_dead = ix->ndead > 0;
	WR_UNLOCK(ix);
	mg->have = false;
}

/* Merges the plan, then removes what it replaced */
static void
merge_run(SiftIdx *ix, const Plan *plan)
{
	MemoryContext cx = op_begin(),
				old = MemoryContextSwitchTo(cx);
	int64	   *dead;
	size_t		ndead;
	Merged		mg = {0};
	SiftRel    *msr;
	Relation	mrel = sift_rel_create(ix->name, ix->dir, ix->man.dpp, &msr);
	int			oldg[SIFT_MAX_SEGMENTS];
	uint32		oldblocks[SIFT_MAX_SEGMENTS];
	uint32		oldlog;
	int			rc;

	RD_LOCK(ix);
	ndead = ix->ndead;
	dead = malloc((ndead + 1) * sizeof(int64));
	memcpy(dead, ix->dead, ndead * sizeof(int64));
	oldlog = ix->man.logno;
	RD_UNLOCK(ix);
	rc = SIFT_TRY(merge_write(ix, plan, mrel, dead, ndead, &mg));
	sift_rel_destroy(mrel);
	if (rc == 0)
	{
		pthread_mutex_lock(&ix->wmu);
		rc = SIFT_TRY(publish_merge(ix, plan, &mg, oldg, oldblocks));
		pthread_mutex_unlock(&ix->wmu);
	}
	free(dead);
	free(mg.dropped);
	MemoryContextSwitchTo(old);
	op_end(cx);
	if (rc != 0)
	{
		char		msg[1024];

		memcpy(msg, sift_err_msg, sizeof(msg));
		if (mg.have)
		{
			char		path[600];

			sift_unmap(mg.g, mg.blocks);
			path_of(ix, path, sizeof(path), mg.ms.file);
			unlink(path);
		}
		fail("%s", msg);
	}
	for (int i = 0; i < plan->n; i++)
	{
		char		path[600];

		sift_unmap(oldg[i], oldblocks[i]);
		path_of(ix, path, sizeof(path), plan->src[i].file);
		unlink(path);
	}
	{
		char		path[600];

		log_path(ix, oldlog, path, sizeof(path));
		unlink(path);
	}
}

void
merge_sets(SiftIdx *ix, bool full)
{
	pthread_mutex_lock(&ix->mergemu);
	for (;;)
	{
		Plan	   *plan = malloc(sizeof(Plan));
		int			rc;

		if (ix->dropped)
		{
			free(plan);
			break;
		}
		pick(ix, full, plan);
		if (plan->n == 0)
		{
			free(plan);
			break;
		}
		rc = SIFT_TRY(merge_run(ix, plan));
		free(plan);
		if (rc != 0)
		{
			char		msg[1024];

			pthread_mutex_unlock(&ix->mergemu);
			memcpy(msg, sift_err_msg, sizeof(msg));
			fail("%s", msg);
		}
		if (full)
			break;
	}
	pthread_mutex_unlock(&ix->mergemu);
}

int
sx_merge(SiftIdx *ix, int full, char *err, size_t errlen)
{
	int			rc;

	thread_init();
	rc = SIFT_TRY(merge_sets(ix, full));
	return finish_call(rc, err, errlen);
}

/* ------------------------------------------------ background thread */

static void
merge_pending(SiftIdx *ix, void *arg)
{
	if (__atomic_exchange_n(&ix->want_merge, false, __ATOMIC_ACQ_REL))
		if (SIFT_TRY(merge_sets(ix, false)) != 0)
			snprintf(ix->merge_err, sizeof(ix->merge_err), "%s", sift_err_msg);
}

static void *
bg_main(void *arg)
{
	thread_init();
	pthread_mutex_lock(&bg_mu);
	while (!bg_stop)
	{
		if (!bg_kicked)
		{
			pthread_cond_wait(&bg_cv, &bg_mu);
			continue;
		}
		bg_kicked = false;
		pthread_mutex_unlock(&bg_mu);
		registry_each(merge_pending, NULL);
		pthread_mutex_lock(&bg_mu);
	}
	pthread_mutex_unlock(&bg_mu);
	return NULL;
}

/* A drain published: the index may now have segments to merge */
void
merge_kick(SiftIdx *ix)
{
	__atomic_store_n(&ix->want_merge, true, __ATOMIC_RELEASE);
	pthread_mutex_lock(&bg_mu);
	if (!bg_started)
		bg_started = pthread_create(&bg_thread, NULL, bg_main, NULL) == 0;
	bg_kicked = true;
	pthread_cond_signal(&bg_cv);
	pthread_mutex_unlock(&bg_mu);
}

void
merge_stop(void)
{
	pthread_mutex_lock(&bg_mu);
	bg_stop = true;
	pthread_cond_signal(&bg_cv);
	pthread_mutex_unlock(&bg_mu);
	if (bg_started)
		pthread_join(bg_thread, NULL);
	bg_started = false;
}
