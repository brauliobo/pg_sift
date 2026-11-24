/*
 * col/f2/f2_v6_scan_table.c - the scan of a sift_table for the aggregate (f2_v6_scan.c): the whole table in one pass, the begin of a parallel run, and the pairs of an unnested array column.
 */
/* check-memory: allow-allocs every array here follows the keys and aggregates of one statement, not the rows of the table */
#include "postgres.h"

#include "agg/v6/v6.h"
#include "agg/v6/par/run.h"
#include "agg/v6/v6_ext.h"
#include "agg/v6/v6_pipe.h"
#include "agg/v6/v6_star_run.h"
#include "col/f2/f2.h"
#include "col/f2/f2_store_scan.h"
#include "col/f2/f2_v6_preagg.h"
#include "col/f2/f2_v6_scan_int.h"
#include "col/f2/f2_v6_unnest_run.h"

void
sift_f2_v6_scan_table_morsels(V6Scan *s, Relation rel, const SiftStoreQuals *quals, Snapshot snap)
{
	SiftF2Store *store = sift_f2_store_open(rel);
	SiftStoreScanOpts *o = palloc(sizeof(SiftStoreScanOpts));

	s->host = rel;
	s->is_table = true;
	sift_f2_v6_scan_setup(s, store->schema->cols, store->att_first, NULL);
	s->leaves = sift_f2_v6_scan_leaves(s);
	*o = (SiftStoreScanOpts) {.rel = rel,.store = store,.snap = snap,.preds = quals->preds,.npreds = quals->npreds,.leaves = s->leaves,.nleaves = s->ncols,
		.nan_leaves = sift_f2_v6_scan_nan_leaves(s, store->ncols)};
	s->store_scan = sift_f2_store_scan_open(o);
	s->unsatisfiable = !quals->possible;
}

static int
unnest_slot(void *arg, const SiftV6Col *c)
{
	V6Scan	   *s = arg;

	return sift_f2_v6_slot_of(s, s->first, c, false);
}

/* a table whose array column is unnested: the pairs of each row go through a pipe whose fact slots are the columns the scan reads and whose element slot follows them */
static void
unnest_begin(V6Scan *s, Relation rel, Snapshot snap)
{
	SiftVecType *type = palloc(Max(s->ncols, 1) * sizeof(SiftVecType));

	for (int i = 0; i < s->ncols; i++)
		type[i] = s->cols[i].vec.type;
	s->unnest = sift_v6_unnest_rows_begin(s->spec, s->group, rel, snap, s->stats, s->ncols, type, unnest_slot, s);
	s->fact = palloc(Max(s->ncols, 1) * sizeof(SiftVec *));
	for (int i = 0; i < s->ncols; i++)
		s->fact[i] = &s->cols[i].vec;
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
	SiftStoreScanOpts o = {.rel = rel,.store = store,.snap = snap,.preds = quals->preds,.npreds = quals->npreds,.skip_gens = skip,.nskip_gens = nskip};

	s->host = rel;
	s->is_table = true;
	sift_f2_v6_scan_setup(s, store->schema->cols, store->att_first, star);
	if (s->spec->ext != NULL && s->spec->ext->unnest)
		unnest_begin(s, rel, snap);
	o.leaves = sift_f2_v6_scan_leaves(s);
	o.nleaves = s->ncols;
	o.nan_leaves = sift_f2_v6_scan_nan_leaves(s, store->ncols);
	s->preagg = quals->possible && s->rows == NULL && s->group != NULL ? sift_v6_preagg_begin(s->spec, s->group, store->att_first, NULL, s->stats) : NULL;
	o.gran_sink = s->preagg != NULL ? sift_v6_preagg_try_granule : NULL;
	o.gran_arg = s->preagg;
	if (quals->possible && next_chunk == NULL && s->pipe == NULL && s->unnest == NULL && (s->group != NULL) != (s->rows != NULL))
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
	if (s->pipe != NULL)
		sift_v6_star_scan_end(star, s->pipe);
	if (s->preagg != NULL)
		sift_v6_preagg_end(s->preagg);
	if (s->unnest != NULL)
		sift_v6_unnest_rows_end(s->unnest);
	if (s->batch_cx != NULL)
		MemoryContextDelete(s->batch_cx);
}

void
sift_v6_scan_table(const SiftV6Spec *spec, SiftV6Group *g, Relation rel, const SiftStoreQuals *quals, Snapshot snap, const uint64 *skip, int nskip,
				   SiftV6ScanStats *stats, SiftV6StarRun *star, SiftV6ChunkFn next_chunk, void *chunk_arg)
{
	V6Scan		s = {.spec = spec,.group = g,.stats = stats};

	sift_f2_v6_scan_table_run(&s, rel, quals, snap, skip, nskip, star, next_chunk, chunk_arg);
}
