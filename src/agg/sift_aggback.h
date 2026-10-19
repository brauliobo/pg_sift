/*
 * sift_aggback.h - the interface between the aggregate scan (sift_agg.c) and the backends that can answer a unit of it without the CPU kernels
 * (docs/GPU.md). A backend sees a narrow view of the scan (SiftAggScan), is offered every unit the source returns, and hands the exact sums of
 * the units it took back through sift_agg_fold_sums, which puts them in the group table the way the pre-aggregates of a unit are put.
 *
 * Life of a backend in a scan:
 *   init      once per process, from sift_agg_init: its settings (GUCs). Nothing else: no device, no memory.
 *   begin     once per scan, before any worker is launched: NULL when the backend has nothing to say (it is off), otherwise its state for the
 *             scan, which may be inactive (the shape or the build is not eligible; explain says why). *exclusive: the leader runs the scan
 *             alone (a scan that uses the backend launches no workers).
 *   run_unit  for each unit after the pre-aggregates: true when the backend took it (its rows are counted or queued), false to let the CPU
 *             kernels scan it, into the same group table. It may fold queued units at any call.
 *   stopped   the backend failed and the scan is to be run again without it: the scan loops stop at once.
 *   finish    the end of the scan: the units still queued run. True: start the scan again on the CPU (the group table is reset by the caller,
 *             run_unit is not called for it).
 *   explain   the lines of EXPLAIN ANALYZE; n_units is the units of the scan.
 *
 * The CPU is the default: a scan whose backends all return NULL from begin runs the kernels of sift_agg.c, unchanged, and sift_agg_cpu_backend is
 * what the scan names then. Every callback but init and begin is called only with the state begin returned.
 */
#ifndef SIFT_AGGBACK_H
#define SIFT_AGGBACK_H

#include "commands/explain.h"
#include "utils/rel.h"

#include "sift_aggspec.h"
#include "sift_colsrc.h"
#include "sift_compat.h"

#if PG_VERSION_NUM >= 180000
#include "commands/explain_state.h"
#endif

/* What a backend sees of a scan. Valid for the scan; the pointers are the scan's own. */
typedef struct SiftAggScan
{
	void	   *host;			/* the scan: the argument of sift_agg_fold_sums */
	const SiftAggSpec *spec;
	const SiftSrcLayout *lay;
	int			nbase;			/* leaves of the source: the ones after are the spec's virtual leaves */
	SiftSrcSource *src;
	Relation	index;
	const bool *pushed;			/* per spec->pred: the source applies it ... */
	const SiftSrcFilter *pflt;	/* ... and this is the filter */
	bool		row_scan;		/* a plain row level scan of an index: no fused arms, no search, no top-N */
	MemoryContext cxt;			/* the scan */
} SiftAggScan;

/*
 * The exact result of a unit, for the groups that have rows. keys: the histogram-like description of the key slots (ngroups, key[], dict, as
 * the pre-aggregates have it), NULL when the scan has no key (one group, kept[0] is 0). rows[j]: the rows of group kept[j]; sum[ai][j]: the sum of
 * aggregate ai in it, sum[ai] NULL for an aggregate that only counts.
 */
typedef struct SiftAggUnitSums
{
	const SiftSrcHist *keys;
	int			nkept;
	const uint32 *kept;
	const int64 *rows;
	sift_i128 *const *sum;
} SiftAggUnitSums;

extern void sift_agg_fold_sums(void *host, const SiftAggUnitSums *r);

typedef struct SiftAggBackend
{
	const char *name;
	void		(*init) (void);
	void	   *(*begin) (const SiftAggScan *scan, bool *exclusive);
	bool		(*run_unit) (void *state, const SiftSrcUnit *unit);
	bool		(*stopped) (const void *state);
	bool		(*finish) (void *state);
	void		(*explain) (const void *state, ExplainState *es, int64 n_units);
} SiftAggBackend;

extern const SiftAggBackend sift_agg_cpu_backend;
extern const SiftAggBackend sift_agg_gpu_backend;		/* sift_agggpu.c: Vulkan; settings only in a build without it */

extern void sift_aggbe_init(void);
/* The first backend that answers begin; the CPU, with a NULL state, when none does */
extern const SiftAggBackend *sift_aggbe_begin(const SiftAggScan *scan, void **state, bool *exclusive);

/* A NULL state is the CPU: nothing is taken, nothing stopped, nothing to say */
static inline bool
sift_aggbe_run_unit(const SiftAggBackend *be, void *state, const SiftSrcUnit *unit)
{
	return state != NULL && be->run_unit(state, unit);
}

static inline bool
sift_aggbe_stopped(const SiftAggBackend *be, const void *state)
{
	return state != NULL && be->stopped(state);
}

static inline bool
sift_aggbe_finish(const SiftAggBackend *be, void *state)
{
	return state != NULL && be->finish(state);
}

static inline void
sift_aggbe_explain(const SiftAggBackend *be, const void *state, ExplainState *es, int64 n_units)
{
	if (state != NULL)
		be->explain(state, es, n_units);
}

#endif							/* SIFT_AGGBACK_H */
