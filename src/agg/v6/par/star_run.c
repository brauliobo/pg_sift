/*
 * agg/v6/par/star_run.c - the star join on the compute threads (agg/v6/par/star_run.h): what a thread does with a granule of the window, and the groups the leader folds into the statement's.
 */
#include "postgres.h"

#include "agg/v6/par/star.h"
#include "agg/v6/par/star_run.h"

#define SCRATCH_BYTES	(8u << 20)
#define FACT_COLS_MAX	32

typedef struct StarRun
{
	const SiftV6Spec *spec;
	SiftV6Group *g;
	const SiftV6Pipe *pipe;
	SiftParWordShape sh;
	SiftParStar *st;
	int			nfact;
	int			leaf[FACT_COLS_MAX];
} StarRun;

static void
star_prepare(void *arg, MemoryContext cx, int nthreads)
{
	StarRun    *r = arg;

	r->st = sift_par_star_new(cx, &r->sh, r->pipe, nthreads);
}

static bool
star_fill(void *arg, int worker, const SiftPinSlot *sl, SiftArena *scratch, SiftParGroupBatch *out, SiftErr *err)
{
	const StarRun *r = arg;
	ColView		cv[FACT_COLS_MAX];
	SiftParCol	col[FACT_COLS_MAX];
	uint64	   *active;
	uint32		nact;

	if (!sift_v6_pin_slot_cols(sl, scratch, err, r->leaf, r->nfact, NULL, cv, &active, &nact))
		return false;
	if (nact == 0)
		return true;
	for (int i = 0; i < r->nfact; i++)
		col[i] = (SiftParCol) {.v = cv[i].i,.isnull = cv[i].isnull};
	return sift_par_star_batch(r->st, worker, col, active, sift_pin_slot_nrows(sl), err, out);
}

static void
star_fold(void *arg, const SiftParGroupRow *row)
{
	const StarRun *r = arg;

	sift_v6_pin_fold_words(r->g, r->spec, &r->sh, row);
}

SiftV6PinRun *
sift_v6_star_pin_begin(const SiftV6Spec *spec, SiftV6Group *g, SiftV6ScanStats *stats, Relation index, Snapshot snap, SiftPinPartOk part_ok, void *part_arg, const SiftV6Pipe *pipe,
				   const SiftV6PinCol *cols, int ncols)
{
	StarRun    *r;
	SiftV6PinDesc d;

	if (pipe == NULL || ncols > FACT_COLS_MAX)
		return NULL;
	for (int i = 0; i < ncols; i++)
		if (!cols[i].plain_int)
			return NULL;
	r = palloc0(sizeof(StarRun));
	if (!sift_par_star_shape(spec, &r->sh))
	{
		pfree(r);
		return NULL;
	}
	r->spec = spec;
	r->g = g;
	r->pipe = pipe;
	r->nfact = ncols;
	for (int i = 0; i < ncols; i++)
		r->leaf[i] = cols[i].leaf;
	d = (SiftV6PinDesc) {.name = "star",.gs = r->sh.gs,.leaf = r->leaf,.nleaves = ncols,.scratch_bytes = SCRATCH_BYTES,.prepare = star_prepare,.fill = star_fill,.fold = star_fold,.arg = r};
	return sift_v6_pinrun_new(&d, spec, stats, index, snap, part_ok, part_arg);
}
