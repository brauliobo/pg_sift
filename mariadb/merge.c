/*
 * merge.c - tiered merge of segments and the background thread that runs it.
 *
 * Policy. The shared one (sift_merge_pick, src/bm25/sift_merge.c): segments of similar size, tiers from 1 MiB, merge when `merge_fanin`
 * of a tier have piled up (every drain adds a small one), so a document is rewritten once per tier and the number of segments stays
 * logarithmic in the size of the index. This host adds its own dead documents (they are not in the liveness bits): an index whose
 * dead documents exceed a quarter of its documents is merged whole, sx_merge(full) always.
 * Streaming. Each source segment becomes a sorted run in a temporary file and the runs are merged page by page (sift_run_from_segment,
 * sift_runs_write): memory does not depend on the size of the segments, only on how many are merged at once.
 * Concurrency. The sources are read through a private mapping of their files (the merge clears liveness bits of the
 * documents it drops in its copy only), so queries on the index go on; the merge holds no lock but `mergemu` until it
 * publishes: the manifest and log are written, then the segment table, the document tables and the slots are swapped in
 * under the write lock and the old files are unlinked. A crash before the manifest rename leaves the old state (the new
 * file is removed at open), after it the new state (the old files are removed at open).
 */
#include "ix.h"

#include <unistd.h>

#include "sift_cmp.h"
#include "sift_merge.h"
#include "sift_runs.h"

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

/* The segments to merge now, copied from the manifest; plan->n = 0 when there is nothing to do */
static void
pick(SiftIdx *ix, bool full, Plan *plan)
{
	SiftMergePolicy pol = {.tier_base = (uint64) 1 << 20,.fanin = ix->fanin};
	SiftDirSnapshot snap = {.nsegs = 0};
	SiftSegEntry sel[SIFT_MAX_SEGMENTS];
	bool		force;
	int			n;

	plan->n = 0;
	RD_LOCK(ix);
	force = full || (ix->ndead > 64 && ix->ndead * 4 > ix->ndoc);
	snap.nsegs = ix->man.nsegs;
	for (int i = 0; i < snap.nsegs; i++)
	{
		snap.segs[i] = ix->segs[i].e;
		if (force && ix->ndead > 0)		/* the engine's dead documents are not in the liveness bits: any segment may hold some */
			snap.segs[i].flags |= SIFT_SEG_DIRTY;
	}
	n = sift_merge_pick(ix->rel, &pol, &snap, force, sel);
	for (int i = 0; i < n; i++)
		for (int j = 0; j < snap.nsegs; j++)
			if (ix->segs[j].e.gen == sel[i].gen)
				plan->src[plan->n++] = ix->segs[j];
	RD_UNLOCK(ix);
}

typedef struct Merged
{
	ManSeg		ms;
	bool		have;
	int			g;
	uint32		blocks;
	int64	   *dropped;		/* the dead documents the segments still held */
	size_t		ndropped,
				capdropped;
} Merged;

/* Deletes from the segment the dead documents it still holds (the ones of its range of superblocks) and notes them */
static void
drop_dead(SiftSegment *seg, const SiftRel *sr, const SiftSegEntry *e, const int64 *dead, size_t ndead, Merged *out)
{
	int64		end = (int64) (e->sb_hi + 1) * SIFT_SB_PAGES * sr->dpp;

	for (size_t i = lower_bound(dead, ndead, (int64) e->sb_lo * SIFT_SB_PAGES * sr->dpp); i < ndead && dead[i] < end; i++)
	{
		if (!sift_seg_delete_key(seg, sift_docid_key(sr, dead[i])))
			continue;
		if (out->ndropped == out->capdropped)
			out->dropped = realloc(out->dropped, (out->capdropped = out->capdropped ? out->capdropped * 2 : 1024) * sizeof(int64));
		out->dropped[out->ndropped++] = dead[i];
	}
}

/* Merges the segments of the plan into one file, mapped but not part of the index yet */
static void
merge_write(SiftIdx *ix, const Plan *plan, Relation mrel, const int64 *dead, size_t ndead, Merged *out)
{
	MemoryContext cx = CurrentMemoryContext;
	SiftScoreRef ref = {ix->sr->opts.k1, ix->sr->opts.b, 0};
	SiftRun   **runs = palloc(plan->n * sizeof(SiftRun *));
	int			nruns = 0;
	SiftRel    *msr = mrel->store;
	SiftWriteTarget *wt;
	uint64		gen;
	int			slot;

	for (int i = 0; i < plan->n; i++)
	{
		char		path[600];
		SiftSegment *seg;
		SiftRun    *r;

		path_of(ix, path, sizeof(path), plan->src[i].file);
		sift_slot_map(msr, plan->src[i].slot, path);
		seg = sift_seg_open(mrel, &plan->src[i].e, cx);
		drop_dead(seg, msr, &plan->src[i].e, dead, ndead, out);
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
	qsort(out->dropped, out->ndropped, sizeof(int64), sift_cmp_i64);
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
		if (!bsearch(&ix->ids[i], mg->dropped, mg->ndropped, sizeof(int64), sift_cmp_i64))
		{
			(*ids)[n] = ix->ids[i];
			(*pks)[n++] = ix->pks[i];
		}
	for (size_t i = 0; i < ix->ndead; i++)
		if (!bsearch(&ix->dead[i], mg->dropped, mg->ndropped, sizeof(int64), sift_cmp_i64))
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
