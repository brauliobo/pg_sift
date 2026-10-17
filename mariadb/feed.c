/*
 * feed.c - a drain becomes one segment; a load (rebuild) replaces the documents of the index.
 *
 * A drain feeds the queue rows above the watermark in ascending seq. Nothing is visible, nothing is locked and nothing
 * is written to the manifest until sx_feed_end: it writes the segment file, appends the events to the log and replaces
 * MANIFEST (the commit point), then swaps the result into the index under its write lock. A load feeds every row of the
 * table (op 'R') and replaces all segments, the log and the docid space (docbase) in the same atomic step.
 * Drains are idempotent: a row at or below the watermark is skipped, so a drain repeated after the commit applies nothing.
 */
#include "postgres.h"

#include <fcntl.h>
#include <unistd.h>

#include "utils/memutils.h"

#include "sift_analyze.h"
#include "sift_build.h"
#include "ix.h"

#define BUILD_BUDGET	((Size) 512 << 20)

struct SiftFeed
{
	SiftIdx    *ix;
	MemoryContext cx;
	SiftAnalyzer *an;
	SiftInvBuf *inv;
	SiftRun   **runs;
	int			nruns,
				maxruns;
	Ev		   *ev;
	size_t		nev,
				capev;
	int64		last;			/* highest seq added */
	int64		watermark;		/* of a load: the queue position the table snapshot corresponds to */
	bool		load;
	int64		ndocs,
				nloaded;
	SiftWriteTarget *wt;
	int			slot;			/* reserved slot of the segment, else -1 */
	int			g;				/* mapped but unpublished segment, else -1 */
	uint32		blocks;
	ManSeg		ms;
	SiftScoreRef ref;
};

static void
feed_init(SiftFeed *f)
{
	SiftAnalyzerConfig cfg;
	bool		pos = f->ix->sr->opts.positions;

	f->cx = AllocSetContextCreate(TopMemoryContext, "sift feed", ALLOCSET_DEFAULT_SIZES);
	MemoryContextSwitchTo(f->cx);
	sift_analyzer_config_from_options(&f->ix->sr->opts, &cfg);
	f->an = sift_analyzer_create(&cfg, f->cx);
	sift_analyzer_set_positions(f->an, pos);
	f->inv = sift_inv_create(BUILD_BUDGET, f->cx, pos);
	f->maxruns = 16;
	f->runs = palloc(f->maxruns * sizeof(SiftRun *));
	f->ref = (SiftScoreRef) {f->ix->sr->opts.k1, f->ix->sr->opts.b, 0};
	f->slot = f->g = -1;
}

SiftFeed *
sx_feed_begin(SiftIdx *ix, int64_t watermark, char *err, size_t errlen)
{
	SiftFeed   *f = calloc(1, sizeof(SiftFeed));
	int			rc;

	thread_init();
	f->ix = ix;
	f->load = watermark >= 0;
	f->watermark = watermark;
	f->last = ix->man.drained;
	if (__atomic_exchange_n(&ix->feeding, true, __ATOMIC_ACQ_REL))
	{
		snprintf(sift_err_msg, sizeof(sift_err_msg), "sift: another drain of index \"%s\" is running", ix->name);
		rc = -1;
	}
	else if ((rc = SIFT_TRY(feed_init(f))) != 0)
		__atomic_store_n(&ix->feeding, false, __ATOMIC_RELEASE);
	MemoryContextSwitchTo(TopMemoryContext);
	finish_call(rc, err, errlen);
	if (rc != 0)
	{
		free(f);
		return NULL;
	}
	return f;
}

static void
feed_spill(SiftFeed *f)
{
	if (f->nruns + 1 >= f->maxruns)
	{
		f->maxruns *= 2;
		f->runs = repalloc(f->runs, f->maxruns * sizeof(SiftRun *));
	}
	f->runs[f->nruns++] = sift_inv_spill(f->inv);
}

static void
feed_event(SiftFeed *f, int64 docid, int64 pk, char op)
{
	if (f->nev == f->capev)
		f->ev = realloc(f->ev, (f->capev = f->capev ? f->capev * 2 : 4096) * sizeof(Ev));
	f->ev[f->nev++] = (Ev) {docid, pk, op};
}

static void
feed_doc(SiftFeed *f, int64 docid, const char *text, size_t len)
{
	SiftTermFreq *terms;
	uint32		dl;
	ItemPointerData tid;
	int			n;

	sift_key_to_tid(sift_docid_key(f->ix->sr, docid), &tid);
	n = sift_analyze_doc(f->an, text, len, &terms, &dl);
	sift_inv_add(f->inv, &tid, terms, n, dl);
	sift_analyzer_reset(f->an);
	f->ndocs++;
	if (sift_inv_full(f->inv))
		feed_spill(f);
}

static void
feed_one(SiftFeed *f, int64 seq, char op, int64 pk, const char *text, size_t len)
{
	if (f->load)
	{
		if (op != 'R')
			fail("sift: a rebuild takes rows of the table (op 'R'), not '%c'", op);
		if (text == NULL)
			return;
		feed_event(f, ++f->nloaded, pk, 'I');
		feed_doc(f, f->nloaded, text, len);
		return;
	}
	if (seq <= f->ix->man.drained)
		return;
	if (seq <= f->last)
		fail("sift: queue rows must be fed in ascending seq (%lld after %lld)", (long long) seq, (long long) f->last);
	if (op != 'I' && op != 'D' && op != 'B')
		fail("sift: unknown queue operation '%c'", op);
	f->last = seq;
	if (op == 'B')
		return;
	if (op == 'I' && text == NULL)
		op = 'D';				/* a row without text has no document: it only retires the previous version */
	feed_event(f, seq + f->ix->man.docbase, pk, op);
	if (op == 'I')
		feed_doc(f, seq + f->ix->man.docbase, text, len);
}

int
sx_feed_add(SiftFeed *f, int64_t seq, char op, int64_t pk, const char *text, size_t len, char *err, size_t errlen)
{
	int			rc;

	thread_init();
	MemoryContextSwitchTo(f->cx);
	rc = SIFT_TRY(feed_one(f, seq, op, pk, text, len));
	MemoryContextSwitchTo(TopMemoryContext);
	return finish_call(rc, err, errlen);
}

/* The segment of the documents fed: written, synced, named and mapped, not yet part of the index */
static void
feed_write_segment(SiftFeed *f)
{
	SiftIdx    *ix = f->ix;
	uint64		gen = gen_take(ix);

	f->slot = slot_reserve(ix);
	f->wt = sift_wt_begin_at(ix->sr, f->slot, false);
	if (f->nruns == 0)
		sift_inv_write(f->inv, f->wt, gen, &f->ref, &f->ms.e);
	else
	{
		if (!sift_inv_empty(f->inv))
			feed_spill(f);
		sift_runs_write(f->runs, f->nruns, f->wt, gen, &f->ref, &f->ms.e);
	}
	sift_wt_finish(f->wt);
	rename_segment(ix, f->wt, gen, &f->ms, &f->g, &f->blocks);
	f->wt = NULL;
}

static void
publish_drain(SiftFeed *f)
{
	SiftIdx    *ix = f->ix;
	Manifest	m = ix->man;
	ManSeg		segs[SIFT_MAX_SEGMENTS];
	bool		have = f->ndocs > 0;

	memcpy(segs, ix->segs, m.nsegs * sizeof(ManSeg));
	if (have)
	{
		if (m.nsegs >= MAX_SEGS)
			fail("sift: the index has %d segments: merge it", MAX_SEGS);
		segs[m.nsegs++] = f->ms;
	}
	if (f->nev)
		append_events(ix, f->ev, f->nev);
	m.log_len += f->nev * sizeof(Ev);
	m.drained = f->last;
	m.version++;
	write_manifest(ix, &m, segs);
	WR_LOCK(ix);
	ix->man = m;
	memcpy(ix->segs, segs, m.nsegs * sizeof(ManSeg));
	if (have)
	{
		sift_slot_attach(ix->sr, f->ms.slot, f->g, f->blocks);
		ix->bytes += (int64) f->blocks * BLCKSZ;
		ix->slot_reserved[f->ms.slot] = false;
	}
	for (size_t i = 0; i < f->nev; i++)
		apply_event(ix, f->ev[i].docid, f->ev[i].pk, f->ev[i].op);
	finish_events(ix);
	delta_drop(ix);
	WR_UNLOCK(ix);
	f->slot = f->g = -1;
}

/* Every segment, the log and the documents are replaced by the loaded ones */
static void
publish_load(SiftFeed *f)
{
	SiftIdx    *ix = f->ix;
	Manifest	m = ix->man;
	Manifest	old = ix->man;
	ManSeg		oldsegs[SIFT_MAX_SEGMENTS];
	int			oldg[SIFT_MAX_SEGMENTS];
	uint32		oldblocks[SIFT_MAX_SEGMENTS];
	uint64		len = f->nev * sizeof(Ev);
	bool		have = f->ndocs > 0;
	char		path[600];
	int			fd;

	memcpy(oldsegs, ix->segs, old.nsegs * sizeof(ManSeg));
	m.docbase = f->nloaded;
	m.drained = f->watermark;
	m.logno++;
	m.log_len = len;
	m.nsegs = have;
	m.version++;
	log_path(ix, m.logno, path, sizeof(path));
	fd = open(path, O_CREAT | O_TRUNC | O_WRONLY, 0644);
	if (fd < 0)
		fail("sift: could not create %s: %s", path, strerror(errno));
	write_all(fd, f->ev, len, path);
	if (fsync(fd) != 0)
		fail("sift: could not sync %s: %s", path, strerror(errno));
	close(fd);
	write_manifest(ix, &m, &f->ms);
	WR_LOCK(ix);
	for (uint32 i = 0; i < old.nsegs; i++)
	{
		oldg[i] = ix->sr->gslot[oldsegs[i].slot];
		oldblocks[i] = ix->sr->slot_blocks[oldsegs[i].slot];
		sift_slot_detach(ix->sr, oldsegs[i].slot);
	}
	ix->man = m;
	reset_docs(ix);
	if (have)
	{
		ix->segs[0] = f->ms;
		sift_slot_attach(ix->sr, f->ms.slot, f->g, f->blocks);
		ix->slot_reserved[f->ms.slot] = false;
	}
	ix->bytes = have ? (int64) f->blocks * BLCKSZ : 0;
	for (size_t i = 0; i < f->nev; i++)
		apply_event(ix, f->ev[i].docid, f->ev[i].pk, f->ev[i].op);
	finish_events(ix);
	ix->epoch++;
	delta_drop(ix);
	WR_UNLOCK(ix);
	f->slot = f->g = -1;
	for (uint32 i = 0; i < old.nsegs; i++)
	{
		sift_unmap(oldg[i], oldblocks[i]);
		path_of(ix, path, sizeof(path), oldsegs[i].file);
		unlink(path);
	}
	log_path(ix, old.logno, path, sizeof(path));
	unlink(path);
}

static void
feed_commit(SiftFeed *f, int64 *rows)
{
	SiftIdx    *ix = f->ix;

	*rows = f->nev;
	if (!f->load && f->nev == 0 && f->last <= ix->man.drained)
		return;
	if (f->ndocs > 0)
		feed_write_segment(f);
	if (f->load)
	{
		pthread_mutex_lock(&ix->mergemu);	/* a merge in flight would publish segments of the documents just replaced */
		LOCKED(ix, publish_load(f));
		pthread_mutex_unlock(&ix->mergemu);
	}
	else
		LOCKED(ix, publish_drain(f));
}

static void
feed_release(SiftFeed *f)
{
	SiftIdx    *ix = f->ix;
	char		path[600];

	if (f->wt)
		sift_wt_abort(f->wt);
	if (f->g >= 0)
	{
		sift_unmap(f->g, f->blocks);
		path_of(ix, path, sizeof(path), f->ms.file);
		unlink(path);
	}
	if (f->slot >= 0)
		slot_release(ix, f->slot);
	MemoryContextSwitchTo(TopMemoryContext);
	MemoryContextDelete(f->cx);
	__atomic_store_n(&ix->feeding, false, __ATOMIC_RELEASE);
	free(f->ev);
	free(f);
}

int
sx_feed_end(SiftFeed *f, int64_t *rows, char *err, size_t errlen)
{
	SiftIdx    *ix = f->ix;
	int			rc;

	thread_init();
	MemoryContextSwitchTo(f->cx);
	rc = SIFT_TRY(feed_commit(f, rows));
	feed_release(f);
	if (rc == 0)
		merge_kick(ix);
	return finish_call(rc, err, errlen);
}

void
sx_feed_abort(SiftFeed *f)
{
	thread_init();
	feed_release(f);
}
