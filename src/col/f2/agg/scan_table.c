/*
 * col/f2/agg/scan_table.c - the scan of a sift_table for the aggregate (f2_v6_scan.c): the whole table in one pass, the begin of a parallel run, and the pairs of an unnested array column.
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
#include "col/f2/agg/unnest/run.h"

void
sift_f2_v6_scan_table_morsels(V6Scan *s, Relation rel, const SiftStoreQuals *quals, Snapshot snap)
{
	SiftF2Store *store = sift_f2_store_open(rel);
	SiftStoreScanOpts *o = palloc(sizeof(SiftStoreScanOpts));

	s->host = rel;
	s->is_table = true;
	s->rows_rel = rel;
	s->rows_snap = snap;
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

/* the spec of the unnest of a table: its quals are decided by the scan of the store (the preds), not by the batches of the unnest, which have no index to bind them to */
static const SiftV6Spec *
unnest_spec(const SiftV6Spec *spec)
{
	SiftV6Spec *copy = palloc(sizeof(SiftV6Spec));
	SiftV6Ext  *x = palloc(sizeof(SiftV6Ext));

	*copy = *spec;
	*x = *spec->ext;
	x->where = NIL;
	copy->ext = x;
	return copy;
}

/* the slot of a lane (a leaf decoded with the column before it): the pairs gather every fact slot, and a lane has no value of a row */
static uint64 lane_valid[SIFT_VEC_WORDS];
static SiftVec lane_vec = {.valid = lane_valid,.type = SVEC_I64};

/* a table whose array column is unnested: the pairs of each row go through a pipe whose fact slots are the columns the scan reads and whose element slot follows them */
static void
unnest_begin(V6Scan *s, Relation rel, Snapshot snap)
{
	SiftVecType *type = palloc(Max(s->ncols, 1) * sizeof(SiftVecType));

	for (int i = 0; i < s->ncols; i++)
		type[i] = s->cols[i].lane ? lane_vec.type : s->cols[i].vec.type;
	s->unnest = sift_v6_unnest_rows_begin(unnest_spec(s->spec), s->group, rel, snap, s->stats, s->ncols, type, unnest_slot, s);
	s->fact = palloc(Max(s->ncols, 1) * sizeof(SiftVec *));
	for (int i = 0; i < s->ncols; i++)
		s->fact[i] = s->cols[i].lane ? &lane_vec : &s->cols[i].vec;
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
	s->rows_rel = rel;
	s->rows_snap = snap;
	sift_f2_v6_scan_setup(s, store->schema->cols, store->att_first, star);
	if (s->spec->ext != NULL && s->spec->ext->unnest)
		unnest_begin(s, rel, snap);
	o.leaves = sift_f2_v6_scan_leaves(s);
	o.nleaves = s->ncols;
	o.nan_leaves = sift_f2_v6_scan_nan_leaves(s, store->ncols);
	s->preagg = quals->possible && !s->spec->tid_range && s->rows == NULL && s->group != NULL ? sift_v6_preagg_begin(s->spec, s->group, store->att_first, NULL, s->stats) : NULL;
	o.gran_sink = s->preagg != NULL ? sift_v6_preagg_try_granule : NULL;
	o.gran_arg = s->preagg;
	if (quals->possible && !s->spec->tid_range && next_chunk == NULL && s->pipe == NULL && s->unnest == NULL && (s->group != NULL) != (s->rows != NULL))
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
	sift_f2_v6_member_end(s);
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
