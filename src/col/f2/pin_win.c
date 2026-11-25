/*
 * col/f2/pin_win.c - the window of col/f2/pin_win.h: what the scan hands over, the leader's feed of the slots, and the pure-C filter and decode of a slot that a thread runs.
 */
#include "postgres.h"

#include "col/f2/col_delta/idx.h"
#include "col/f2/part/pin_scan.h"
#include "col/f2/view.h"
#include "col/f2/pin_win.h"
#include "col/f2/scan/par_int.h"
#include "col/granule/cfilt/str_simd.h"
#include "core/memtrack.h"
#include "core/popcount.h"
#include "utils/memutils.h"

#define PIN_WIN_PARTS_MAX	64		/* the parts a window holds the pins of: a scan of more reads the rest on the serial path */
#define PIN_WIN_ITEMS_MAX	(1u << 24)
#define PIN_WIN_RECORD_MAX	(32u << 20)
#define PIN_WIN_PREDS_MAX	32		/* the predicates of a granule: the bits of SiftPinSlot.decided */

typedef struct SiftPinPart
{
	SiftF2PartPin *pin;			/* held until sift_pin_win_end */
	PartPreds	pp;				/* a copy: what it points to lives as long as the scan */
	const SiftColPred *preds;
	int			npreds;
	const SiftColSchema *schema;
	bool		tids_unneeded;
	const void *host;			/* what the scan that listed the part knows it by (SiftPinReplay) */
} SiftPinPart;

typedef struct PinJob
{
	uint32		part;
	uint32		granule;
	uint16		nrows;
	uint32		decided;
	bool		zone_ok;
	int64		zmin;
	int64		zmax;
	uint32		znull;
} PinJob;

struct SiftPinWin
{
	MemoryContext cx;
	MemoryContextCallback cb;	/* releases the pins of the parts with the window, a statement that fails included */
	Relation	rel;
	Snapshot	snap;
	F2Scan	   *scan;			/* the scan of an index that listed the granules */
	SiftPinReplay replay;		/* the scan of a table that did */
	void	   *replay_arg;
	const SiftColPred *tpreds;	/* the predicates of the scan of a table, which every row of a granule must pass */
	int			ntpreds;
	SiftCfiltPred *tcf;			/* tpreds compiled to run on the stored chunks */
	bool	   *tcf_ok;
	bool		tpreds_ok;		/* every one of them runs on the stored chunks */
	bool	   *want;			/* per leaf of the schema: the chunk is read for the granule */
	int			nleaves;
	int			ncols;
	SiftPinPartOk part_ok;
	void	   *part_arg;
	SiftPinPart *parts;
	uint32		nparts;
	PinJob	   *jobs;
	uint32		njobs;
	uint32		cap;
	uint64		rows;
	uint32		refused;
	uint32		head;			/* the granules the scan reads itself before it hands any over */
	int			zone_leaf;		/* the leaf whose zone the jobs keep (-1: none) */
	bool		zones_ok;		/* every job has the zone of it */
	int64		zmin;
	int64		zmax;
	uint32		tid_job;		/* the granule whose addresses tids holds (UINT32_MAX: none) */
	uint64	   *tids;
	uint32		nslots;
	SiftPinSlot slot[SIFT_PIN_WIN_SLOTS_MAX];
};

static void
pin_win_release(void *arg)
{
	const SiftPinWin *w = arg;

	for (uint32 i = 0; i < w->nparts; i++)
		sift_f2_pin_release(w->parts[i].pin);
}

SiftPinWin *
sift_pin_win_new(Relation rel, Snapshot snap, const int *leaves, int nleaves, uint32 nslots, SiftPinPartOk part_ok, void *part_arg)
{
	MemoryContext cx = sift_mem_allocset_create(CurrentMemoryContext, "sift pin window");
	SiftPinWin *w = MemoryContextAllocZero(cx, sizeof(SiftPinWin));

	Assert(nslots >= 1 && nslots <= SIFT_PIN_WIN_SLOTS_MAX);
	w->cx = cx;
	w->cb = (MemoryContextCallback) {pin_win_release, w, NULL};
	MemoryContextRegisterResetCallback(cx, &w->cb);
	w->rel = rel;
	w->snap = snap;
	w->nslots = nslots;
	w->part_ok = part_ok;
	w->part_arg = part_arg;
	w->ncols = 0;
	w->zone_leaf = -1;
	w->tid_job = UINT32_MAX;
	w->zones_ok = true;
	w->zmin = INT64_MAX;
	w->zmax = INT64_MIN;
	w->cap = 64;
	w->jobs = MemoryContextAlloc(cx, sizeof(PinJob) * w->cap);
	w->parts = MemoryContextAllocZero(cx, sizeof(SiftPinPart) * PIN_WIN_PARTS_MAX);
	w->nleaves = nleaves;
	w->want = NULL;
	w->slot[0].slab_cap = 0;
	for (int i = 0; i < nleaves; i++)
		w->ncols = Max(w->ncols, leaves[i] + 1);
	w->want = MemoryContextAllocZero(cx, Max(w->ncols, 1));
	for (int i = 0; i < nleaves; i++)
		w->want[leaves[i]] = true;
	return w;
}

uint32
sift_pin_win_items(const SiftPinWin *w)
{
	return w->njobs;
}

uint32
sift_pin_win_refused(const SiftPinWin *w)
{
	return w->refused;
}

uint64
sift_pin_win_rows(const SiftPinWin *w)
{
	return w->rows;
}

const SiftPinSlot *
sift_pin_win_slot(const SiftPinWin *w, uint32 item)
{
	return &w->slot[item % w->nslots];
}

uint32
sift_pin_slot_nrows(const SiftPinSlot *sl)
{
	return sl->nrows;
}

uint32
sift_pin_slot_job(const SiftPinSlot *sl)
{
	return sl->job;
}

StaticAssertDecl(SIFT_PIN_GRANULE_ROWS == SIFT_GRANULE_ROWS, "the window and the granule agree on the rows of a granule");
StaticAssertDecl(SIFT_PIN_ACTIVE_WORDS == SIFT_CFILT_WORDS + 2, "the bitmap of a filter has the words the window says");
StaticAssertDecl(SIFT_PIN_POINT_ROWS_MAX == SIFT_POINT_ROWS_MAX, "the window and the granule agree on the rows read by their places");

SiftStrMemo *
sift_pin_memo_new(SiftArena *a)
{
	return sift_arena_alloc0(a, sizeof(SiftStrMemo), SIFT_ARENA_ALIGN);
}

void
sift_pin_win_serial_head(SiftPinWin *w, uint32 n)
{
	w->head = n;
}

void
sift_pin_win_track_zone(SiftPinWin *w, int leaf)
{
	Assert(w->njobs == 0);
	w->zone_leaf = leaf;
}

static void
note_zone(SiftPinWin *w, PinJob *job, const SiftZone *zones, uint32 g)
{
	job->zone_ok = zones != NULL && !(zones[g].flags & SIFT_ZONE_NONE);
	if (!job->zone_ok)
	{
		w->zones_ok = false;
		return;
	}
	job->zmin = zones[g].min;
	job->zmax = zones[g].max;
	job->znull = zones[g].nnull;
	w->zmin = Min(w->zmin, job->zmin);
	w->zmax = Max(w->zmax, job->zmax);
}

bool
sift_pin_win_job_zone(const SiftPinWin *w, uint32 job, int64 *min, int64 *max, bool *has_null)
{
	const PinJob *j = &w->jobs[job];

	*min = j->zmin;
	*max = j->zmax;
	*has_null = j->znull > 0;
	return j->zone_ok;
}

bool
sift_pin_win_zone_range(const SiftPinWin *w, int64 *min, int64 *max)
{
	*min = w->zmin;
	*max = w->zmax;
	return w->zones_ok && w->njobs > 0;
}

/* The part of the scan the pin and the predicate set belong to, the first time a granule of it is handed over */
static uint32
part_of(SiftPinWin *w, Relation rel, const SiftColSchema *schema, SiftF2PartPin *pin, const PartPreds *pp, const SiftColPred *preds, int npreds, bool tids_unneeded, const void *host)
{
	SiftPinPart *p;

	if (w->nparts > 0 && w->parts[w->nparts - 1].pp.cf == pp->cf && w->parts[w->nparts - 1].host == host && w->parts[w->nparts - 1].pin->part_gen == pin->part_gen)
		return w->nparts - 1;
	p = &w->parts[w->nparts];
	p->pin = sift_f2_scan_pin(rel, &pin->desc, pin->part_id);
	p->pp = *pp;
	p->preds = preds;
	p->npreds = npreds;
	p->schema = schema;
	p->tids_unneeded = tids_unneeded;
	p->host = host;
	return w->nparts++;
}

/* The granule into the list; false when the window cannot take it */
static bool
list_granule(SiftPinWin *w, Relation rel, const SiftColSchema *schema, SiftF2PartPin *pin, const PartPreds *pp, const SiftColPred *preds, int npreds, bool tids_unneeded, const void *host,
			 uint32 g, uint32 decided)
{
	const SiftGranuleDesc *gd = &pin->gdir[g];
	PinJob		job = {.granule = g,.nrows = gd->nrows,.decided = decided};

	for (int c = 0; c < w->ncols; c++)
		if (w->want[c] && (sift_f2_exc_has(pp->exc, pp->nexc, g, c) || schema->cols[c].path[0] != '\0'))
			return false;
	if (gd->data_len > PIN_WIN_RECORD_MAX || w->njobs >= PIN_WIN_ITEMS_MAX)
		return false;
	if (w->nparts == PIN_WIN_PARTS_MAX && (w->parts[w->nparts - 1].pp.cf != pp->cf || w->parts[w->nparts - 1].host != host || w->parts[w->nparts - 1].pin->part_gen != pin->part_gen))
		return false;
	job.part = part_of(w, rel, schema, pin, pp, preds, npreds, tids_unneeded, host);
	if (w->zone_leaf >= 0)
		note_zone(w, &job, sift_f2_pin_zones(rel, pin, w->zone_leaf), g);
	if (w->njobs == w->cap)
		w->jobs = repalloc(w->jobs, sizeof(PinJob) * (w->cap *= 2));
	w->jobs[w->njobs++] = job;
	w->rows += gd->nrows;
	return true;
}

bool
sift_pin_win_take(SiftPinWin *w, Relation rel, F2Scan *s, const SiftColSchema *schema, SiftF2PartPin *pin, const PartPreds *pp, uint32 g, const bool *decided, const SiftPendDelIndex *px,
				  Snapshot snap)
{
	uint32		mask = 0;

	if (w->head > 0)
	{
		w->head--;
		return false;
	}
	w->refused++;
	if (px != NULL || s->cands != NULL || s->bridge != NULL || s->sink == NULL || s->sink_maybe || s->heap_decide || s->survey || s->npreds > PIN_WIN_PREDS_MAX || sift_cdi_scan_granule_patched(s, g) ||
		!w->part_ok(w->part_arg, &pin->desc))
		return false;
	for (int k = 0; k < s->npreds; k++)
	{
		if (!decided[k] && (!s->cf_ok[k] || sift_f2_exc_has(pp->exc, pp->nexc, g, s->preds[k].col)))
			return false;
		mask |= (uint32) decided[k] << k;
	}
	if (!list_granule(w, rel, schema, pin, pp, s->preds, s->npreds, s->tids_unneeded, NULL, g, mask))
		return false;
	w->scan = s;
	w->refused--;
	return true;
}

void
sift_pin_win_table_preds(SiftPinWin *w, const SiftColSchema *schema, const SiftColPred *preds, int npreds)
{
	w->tpreds = preds;
	w->ntpreds = npreds;
	w->tcf = MemoryContextAlloc(w->cx, Max(npreds, 1) * sizeof(SiftCfiltPred));
	w->tcf_ok = MemoryContextAllocZero(w->cx, Max(npreds, 1));
	w->tpreds_ok = npreds <= PIN_WIN_PREDS_MAX;
	for (int k = 0; w->tpreds_ok && k < npreds; k++)
		w->tpreds_ok = w->tcf_ok[k] = sift_cfilt_compile(&w->tcf[k], &schema->cols[preds[k].col], &preds[k]);
}

/* The predicates of the table scan for the part of granule g, with the dictionaries of the part tested once: false when a chunk the predicates read holds a value of g they cannot judge */
static bool
table_part_preds(SiftPinWin *w, Relation rel, const SiftColSchema *schema, const void *host, const SiftF2PartPin *pin, uint32 g, PartPreds *pp)
{
	const SiftPinPart *last = w->nparts > 0 ? &w->parts[w->nparts - 1] : NULL;

	for (int k = 0; k < w->ntpreds; k++)
		if (sift_f2_exc_has(pp->exc, pp->nexc, g, w->tpreds[k].col))
			return false;
	if (last != NULL && last->host == host && last->pin->part_gen == pin->part_gen)
		pp->cf = last->pp.cf;
	else if (w->ntpreds > 0)
	{
		MemoryContext old = MemoryContextSwitchTo(w->cx);

		pp->cf = memcpy(palloc(w->ntpreds * sizeof(SiftCfiltPred)), w->tcf, w->ntpreds * sizeof(SiftCfiltPred));
		sift_f2_part_cfilt(sift_part_dicts(rel, pin->part), schema, pp->exc, pp->nexc, w->tpreds, w->ntpreds, w->tcf_ok, pp->cf);
		MemoryContextSwitchTo(old);
	}
	return true;
}

bool
sift_pin_win_take_table(SiftPinWin *w, Relation rel, const SiftColSchema *schema, const void *host, SiftF2PartPin *pin, uint32 g, bool eligible, SiftPinReplay replay, void *replay_arg)
{
	static const PartPreds none;
	int			nexc;
	PartPreds	pp = none;

	if (w->head > 0)
	{
		w->head--;
		return false;
	}
	w->refused++;
	if (!eligible || !w->tpreds_ok || pin->col_map != NULL)
		return false;
	pp.exc = sift_f2_pin_exc(rel, pin, &nexc);
	pp.nexc = nexc;
	if (!table_part_preds(w, rel, schema, host, pin, g, &pp) || !list_granule(w, rel, schema, pin, &pp, w->tpreds, w->ntpreds, false, host, g, 0))
		return false;
	w->replay = replay;
	w->replay_arg = replay_arg;
	w->refused--;
	return true;
}

/* A slab of cap bytes for the slot: the old one is given back (the slot is free: no thread reads it) */
static void
slab_for(SiftPinWin *w, SiftPinSlot *sl, Size need)
{
	if (sl->slab_cap >= need)
		return;
	if (sl->slab != NULL)
		pfree(sl->slab);
	sl->slab_cap = Max(need, sl->slab_cap * 2);
	sl->slab = MemoryContextAllocAligned(w->cx, sl->slab_cap, SIFT_ARENA_ALIGN, MCXT_ALLOC_HUGE);
}

bool
sift_pin_win_feed(SiftPinWin *w, uint32 item, uint32 jobno)
{
	const PinJob *job;
	const SiftPinPart *pt;
	SiftPinSlot *sl;
	const SiftGranuleDesc *gd;
	const SiftDvGran *dg;
	bool		want[SIFT_PIN_MAX_LEAVES] = {false};

	if (jobno >= w->njobs)
		return false;
	job = &w->jobs[jobno];
	pt = &w->parts[job->part];
	sl = &w->slot[item % w->nslots];
	gd = &pt->pin->gdir[job->granule];
	dg = pt->pin->dv != NULL ? sift_dv_granule(pt->pin->dv, job->granule) : NULL;
	Assert(w->ncols <= SIFT_PIN_MAX_LEAVES);
	memcpy(want, w->want, w->ncols);
	for (int k = 0; k < pt->npreds; k++)
		if (!((job->decided >> k) & 1))
			want[pt->preds[k].col] = true;
	slab_for(w, sl, gd->data_len);
	sift_part_read_granule_into(w->rel, pt->pin->part, job->granule, want, gd, sl->slab);
	sl->view = sift_granule_open_part(w->rel, pt->pin->part, sl->slab, gd->data_len, gd);
	sl->part = pt;
	sl->granule = job->granule;
	sl->job = jobno;
	sl->nrows = gd->nrows;
	sl->decided = job->decided;
	sl->any_dead = dg != NULL && sift_dv_mask64(dg, w->snap, sl->dead) > 0;
	return true;
}

void
sift_pin_win_release(SiftPinWin *w, uint32 item)
{
}

uint64
sift_pin_win_tid(SiftPinWin *w, uint32 jobno, uint32 row)
{
	if (w->tid_job != jobno)
	{
		const PinJob *job = &w->jobs[jobno];
		const SiftPinPart *pt = &w->parts[job->part];
		const SiftGranuleDesc *gd = &pt->pin->gdir[job->granule];
		bool	   *none = palloc0(Max(pt->pin->part->hdr.ncols, 1));
		char	   *rec = palloc(gd->data_len);
		SiftGranuleView v;

		sift_part_read_granule_into(w->rel, pt->pin->part, job->granule, none, gd, rec);
		v = sift_granule_open_part(w->rel, pt->pin->part, rec, gd->data_len, gd);
		if (w->tids == NULL)
			w->tids = MemoryContextAlloc(w->cx, sizeof(uint64) * SIFT_GRANULE_ROWS);
		sift_granule_tids(&v, w->tids);
		w->tid_job = jobno;
		pfree(rec);
		pfree(none);
	}
	return w->tids[row];
}

void
sift_pin_win_replay(SiftPinWin *w, uint32 jobno)
{
	const PinJob *job = &w->jobs[jobno];
	const SiftPinPart *pt = &w->parts[job->part];
	F2Scan	   *s = w->scan;
	const SiftPartDesc *part;
	bool		tids_unneeded;
	struct SiftPinWin *taken;

	if (pt->host != NULL)
	{
		w->replay(w->replay_arg, pt->host, pt->pin, job->granule);
		return;
	}
	part = s->part;
	tids_unneeded = s->tids_unneeded;
	taken = s->pinwin;
	s->pinwin = NULL;
	s->part = &pt->pin->desc;
	s->tids_unneeded = pt->tids_unneeded;
	PG_TRY();
	{
		if (s->part_sink != NULL)
			s->part_sink(s->sink_arg, s->part, false, &s->tids_unneeded);
		scan_span(w->rel, s, pt->schema, pt->pin, &pt->pp, job->granule, job->granule + 1, NULL, w->snap);
	}
	PG_FINALLY();
	{
		s->pinwin = taken;
		s->part = part;
		s->tids_unneeded = tids_unneeded;
	}
	PG_END_TRY();
}

void
sift_pin_win_end(SiftPinWin *w)
{
	MemoryContextDelete(w->cx);
}

/* ---- the threads: pure C over the slot ---- */

bool
sift_pin_slot_filter_a(const SiftPinSlot *sl, SiftArena *a, SiftErr *err, SiftStrMemo *memo, uint64 *active, uint32 *nactive)
{
	const SiftPinPart *pt = sl->part;
	const int	n = sl->nrows,
				words = (n + 63) / 64;
	uint32		count = 0;

	memset(active, 0, SIFT_PIN_ACTIVE_WORDS * sizeof(uint64));
	memset(active, 0xFF, n / 64 * sizeof(uint64));
	if (n & 63)
		active[n / 64] = (UINT64CONST(1) << (n & 63)) - 1;
	for (int w = 0; sl->any_dead && w < words; w++)
		active[w] &= ~sl->dead[w];
	for (int k = 0; k < pt->npreds; k++)
	{
		uint64		mask[SIFT_PIN_ACTIVE_WORDS];
		SiftCfiltResult res;

		if ((sl->decided >> k) & 1)
			continue;
		if (!sift_granule_filter_compressed_live_a(a, err, memo, &sl->view, &pt->pp.cf[k], active, mask, &res))
			return false;
		if (res == SIFT_CFILT_UNSUPPORTED)
			return sift_err_fill(err, SIFT_ERR_SERIAL, k, 0, "a predicate the stored chunk cannot test"), false;
		if (res == SIFT_CFILT_ALL_FALSE)
			memset(active, 0, words * sizeof(uint64));
		else if (res == SIFT_CFILT_BITS)
			for (int w = 0; w < words; w++)
				active[w] &= mask[w];
	}
	for (int w = 0; w < words; w++)
		count += sift_pc64(active[w]);
	*nactive = count;
	return true;
}

bool
sift_pin_slot_view_a(const SiftPinSlot *sl, SiftArena *a, SiftErr *err, const SiftStrMemo *memo, int leaf, bool codes, const uint16 *rows, int nrows, ColView *out)
{
	return sift_f2_decode_view_a(a, err, memo, &sl->part->schema->cols[leaf], &sl->view, leaf, sl->nrows, codes, rows, nrows, out);
}
