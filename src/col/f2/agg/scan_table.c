/*
 * col/f2/agg/scan_table.c - the scan of a sift_table for the aggregate (f2_v6_scan.c): the whole table in one pass and the begin of a parallel run.
 */
/* check-memory: allow-allocs every array here follows the keys and aggregates of one statement, not the rows of the table */
#include "postgres.h"

#include "agg/engine.h"
#include "agg/par/run.h"
#include "agg/ext.h"
#include "agg/pipe.h"
#include "agg/star/run.h"
#include "col/f2/f2.h"
#include "col/f2/store/scan.h"
#include "col/f2/agg/preagg.h"
#include "col/f2/agg/scan_int.h"

bool *
sift_f2_v6_scan_nan_leaves(const V6Scan *s, int nleaves)
{
	bool	   *nan_ok = palloc0(Max(nleaves, 1) * sizeof(bool));

	for (int i = 0; i < s->ncols; i++)
		nan_ok[s->cols[i].leaf] = s->cols[i].nan_ok;
	return nan_ok;
}

void
sift_f2_v6_scan_table_morsels(V6Scan *s, Relation rel, const SiftStoreQuals *quals, Snapshot snap, SiftV6StarRun *star)
{
	SiftF2Store *store = sift_f2_store_open(rel);
	SiftStoreScanOpts *o = palloc(sizeof(SiftStoreScanOpts));

	s->host = rel;
	s->is_table = true;
	s->rows_rel = rel;
	s->rows_snap = snap;
	sift_f2_v6_scan_setup(s, store->schema->cols, store->att_first, star);
	s->leaves = sift_f2_v6_scan_leaves(s);
	*o = (SiftStoreScanOpts) {.rel = rel,.store = store,.snap = snap,.preds = quals->preds,.npreds = quals->npreds,.leaves = s->leaves,.nleaves = s->ncols,
		.nan_leaves = sift_f2_v6_scan_nan_leaves(s, store->ncols),.grans = &s->grans};
	o->zones = sift_f2_v6_zone_tests(s->spec, rel, &o->nzones, s->stats);
	s->store_scan = sift_f2_store_scan_open(o);
	s->unsatisfiable = !quals->possible;
}

static void
table_par_done(void *arg)
{
	V6Scan	   *s = arg;

	sift_v6_par_end(s->par);
	s->par = NULL;
}

void
sift_f2_v6_scan_table_run(V6Scan *s, Relation rel, const SiftStoreQuals *quals, Snapshot snap, const uint64 *skip, int nskip, SiftV6StarRun *star, SiftV6ChunkFn next_chunk,
						  void *chunk_arg)
{
	const SiftTidBridge *bridge = NULL;
	SiftF2Store *store = sift_f2_store_open(rel);
	SiftStoreScanOpts o = {.rel = rel,.store = store,.snap = snap,.preds = quals->preds,.npreds = quals->npreds,.skip_gens = skip,.nskip_gens = nskip,.grans = &s->grans,
	.tid_range = s->spec->tid_range,.tid_lo = s->spec->tid_lo,.tid_hi = s->spec->tid_hi};

	s->host = rel;
	s->is_table = true;
	s->rows_rel = rel;
	s->rows_snap = snap;
	sift_f2_v6_scan_setup(s, store->schema->cols, store->att_first, star);
	o.leaves = sift_f2_v6_scan_leaves(s);
	o.zones = sift_f2_v6_zone_tests(s->spec, rel, &o.nzones, s->stats);
	o.nleaves = s->ncols;
	o.nan_leaves = sift_f2_v6_scan_nan_leaves(s, store->ncols);
	s->preagg = quals->possible && !s->spec->tid_range && s->rows == NULL && s->group != NULL ? sift_v6_preagg_begin(s->spec, s->group, store->att_first, NULL, s->stats) : NULL;
	o.gran_sink = s->preagg != NULL ? sift_v6_preagg_try_granule : NULL;
	o.gran_arg = s->preagg;
	if (quals->possible && !s->spec->tid_range && next_chunk == NULL && s->pipe == NULL && (s->group != NULL) != (s->rows != NULL))
		s->par = sift_v6_par_begin(s->spec, s->group, s->rows, s->stats, store->att_first, rel, snap, NULL, NULL);
	if (s->par != NULL)
	{
		o.pinwin = sift_v6_par_window(s->par);
		o.pinwin_done = table_par_done;
	}
	if (quals->possible && next_chunk == NULL)
		sift_f2_store_scan_sink(&o, sift_f2_v6_sink, s);
	while (quals->possible && next_chunk != NULL && next_chunk(chunk_arg, &bridge))
	{
		s->stats->chunks++;
		s->stats->bridge_tids += bridge->ntids;
		o.bridge = bridge;
		sift_f2_store_scan_sink(&o, sift_f2_v6_sink, s);
	}
	sift_f2_v6_flush_grans(s);
	if (s->pipe != NULL)
		sift_v6_star_scan_end(star, s->pipe);
	if (s->preagg != NULL)
		sift_v6_preagg_end(s->preagg);
	sift_f2_v6_scan_end(s);
}

void
sift_v6_scan_table(const SiftV6Spec *spec, SiftV6Group *g, Relation rel, const SiftStoreQuals *quals, Snapshot snap, const uint64 *skip, int nskip,
				   SiftV6ScanStats *stats, SiftV6StarRun *star, SiftV6ChunkFn next_chunk, void *chunk_arg)
{
	V6Scan		s = {.spec = spec,.group = g,.stats = stats};

	sift_f2_v6_scan_table_run(&s, rel, quals, snap, skip, nskip, star, next_chunk, chunk_arg);
}
