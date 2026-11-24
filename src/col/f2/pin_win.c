/*
 * col/f2/pin_win.c - the window of col/f2/pin_win.h: what the scan hands over, the leader's feed of the slots, and the pure-C filter and decode of a slot that a thread runs.
 */
#include "postgres.h"

#include "col/f2/f2_col_delta_idx.h"
#include "col/f2/f2_part_pin_scan.h"
#include "col/f2/f2_view.h"
#include "col/f2/pin_win.h"
#include "col/f2/scan/par_int.h"
#include "col/granule/cfilt_str_simd.h"
#include "core/popcount.h"
#include "utils/memutils.h"

#define PIN_WIN_PARTS_MAX	64		/* the parts a window holds the pins of: a scan of more reads the rest on the serial path */
#define PIN_WIN_ITEMS_MAX	(1u << 24)
#define PIN_WIN_RECORD_MAX	(32u << 20)

typedef struct SiftPinPart
{
	SiftF2PartPin *pin;			/* held until sift_pin_win_end */
	PartPreds	pp;				/* a copy: what it points to lives as long as the scan */
	const SiftColPred *preds;
	int			npreds;
	const SiftColSchema *schema;
	bool		tids_unneeded;
} SiftPinPart;

typedef struct PinJob
{
	uint32		part;
	uint32		granule;
	uint16		nrows;
	uint32		decided;
} PinJob;

struct SiftPinWin
{
	MemoryContext cx;
	Relation	rel;
	Snapshot	snap;
	F2Scan	   *scan;
	const SiftColSchema *schema;
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
	uint32		nslots;
	SiftPinSlot slot[SIFT_PIN_WIN_SLOTS_MAX];
};

SiftPinWin *
sift_pin_win_new(Relation rel, Snapshot snap, const int *leaves, int nleaves, uint32 nslots, SiftPinPartOk part_ok, void *part_arg)
{
	MemoryContext cx = AllocSetContextCreate(CurrentMemoryContext, "sift pin window", ALLOCSET_DEFAULT_SIZES);
	SiftPinWin *w = MemoryContextAllocZero(cx, sizeof(SiftPinWin));

	Assert(nslots >= 1 && nslots <= SIFT_PIN_WIN_SLOTS_MAX);
	w->cx = cx;
	w->rel = rel;
	w->snap = snap;
	w->nslots = nslots;
	w->part_ok = part_ok;
	w->part_arg = part_arg;
	w->ncols = 0;
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

/* The part of the scan the pin and the predicate set belong to, the first time a granule of it is handed over */
static uint32
part_of(SiftPinWin *w, Relation rel, F2Scan *s, const SiftColSchema *schema, SiftF2PartPin *pin, const PartPreds *pp)
{
	SiftPinPart *p;

	if (w->nparts > 0 && w->parts[w->nparts - 1].pp.cf == pp->cf && w->parts[w->nparts - 1].pin->part_gen == pin->part_gen)
		return w->nparts - 1;
	p = &w->parts[w->nparts];
	p->pin = sift_f2_scan_pin(rel, &pin->desc, pin->part_id);
	p->pp = *pp;
	p->preds = s->preds;
	p->npreds = s->npreds;
	p->schema = schema;
	p->tids_unneeded = s->tids_unneeded;
	return w->nparts++;
}

bool
sift_pin_win_take(SiftPinWin *w, Relation rel, F2Scan *s, const SiftColSchema *schema, SiftF2PartPin *pin, const PartPreds *pp, uint32 g, const bool *decided, const SiftPendDelIndex *px,
				  Snapshot snap)
{
	const SiftGranuleDesc *gd = &pin->gdir[g];
	PinJob		job = {.granule = g,.nrows = gd->nrows};

	w->refused++;
	if (px != NULL || s->cands != NULL || s->bridge != NULL || s->sink == NULL || s->sink_maybe || s->heap_decide || s->survey || s->npreds > 32 || gd->data_len > PIN_WIN_RECORD_MAX ||
		w->njobs >= PIN_WIN_ITEMS_MAX || sift_cdi_scan_granule_patched(s, g) || !w->part_ok(w->part_arg, &pin->desc))
		return false;
	for (int k = 0; k < s->npreds; k++)
	{
		if (!decided[k] && (!s->cf_ok[k] || sift_f2_exc_has(pp->exc, pp->nexc, g, s->preds[k].col)))
			return false;
		job.decided |= (uint32) decided[k] << k;
	}
	for (int c = 0; c < w->ncols; c++)
		if (w->want[c] && (sift_f2_exc_has(pp->exc, pp->nexc, g, c) || schema->cols[c].path[0] != '\0'))
			return false;
	if (w->nparts == PIN_WIN_PARTS_MAX && (w->parts[w->nparts - 1].pp.cf != pp->cf || w->parts[w->nparts - 1].pin->part_gen != pin->part_gen))
		return false;
	job.part = part_of(w, rel, s, schema, pin, pp);
	if (w->njobs == w->cap)
		w->jobs = repalloc(w->jobs, sizeof(PinJob) * (w->cap *= 2));
	w->jobs[w->njobs++] = job;
	w->refused--;
	w->rows += gd->nrows;
	w->scan = s;
	w->schema = schema;
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
sift_pin_win_feed(void *vw, uint32 item)
{
	SiftPinWin *w = vw;
	const PinJob *job;
	const SiftPinPart *pt;
	SiftPinSlot *sl;
	const SiftGranuleDesc *gd;
	const SiftDvGran *dg;
	bool		want[SIFT_PIN_MAX_LEAVES];

	if (item >= w->njobs)
		return false;
	job = &w->jobs[item];
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
	sl->nrows = gd->nrows;
	sl->decided = job->decided;
	sl->any_dead = dg != NULL && sift_dv_mask64(dg, w->snap, sl->dead) > 0;
	return true;
}

void
sift_pin_win_release(void *vw, uint32 item)
{
}

void
sift_pin_win_replay(void *vw, uint32 item)
{
	SiftPinWin *w = vw;
	const PinJob *job = &w->jobs[item];
	const SiftPinPart *pt = &w->parts[job->part];
	F2Scan	   *s = w->scan;
	const SiftPartDesc *part = s->part;
	const bool	tids_unneeded = s->tids_unneeded;
	struct SiftPinWin *taken = s->pinwin;

	s->pinwin = NULL;
	s->part = &pt->pin->desc;
	s->tids_unneeded = pt->tids_unneeded;
	PG_TRY();
	{
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
	for (uint32 i = 0; i < w->nparts; i++)
		sift_f2_pin_release(w->parts[i].pin);
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

	memset(active, 0, (SIFT_CFILT_WORDS + 2) * sizeof(uint64));
	memset(active, 0xFF, n / 64 * sizeof(uint64));
	if (n & 63)
		active[n / 64] = (UINT64CONST(1) << (n & 63)) - 1;
	for (int w = 0; sl->any_dead && w < words; w++)
		active[w] &= ~sl->dead[w];
	for (int k = 0; k < pt->npreds; k++)
	{
		uint64		mask[SIFT_CFILT_WORDS + 2];
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
