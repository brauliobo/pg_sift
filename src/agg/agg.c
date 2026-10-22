/*
 * agg/agg.c - executor of Custom Scan (SiftAgg) and the GUCs of the aggregate pushdown.
 *
 * The scan reads the units of the column source (agg/colsrc.h), one superblock at a time. For every unit it
 *   1. evaluates the row level predicates to a bitmap (the kernels of agg/kern.h),
 *   2. for a query that unnests an array leaf, evaluates the element level predicates in vectors of SIFT_AGG_VEC elements,
 *      the row bitmap lifted to the elements through their parent ordinals,
 *   3. keeps only the rows that are visible (sift_colsrc_vet: rows on all-visible pages are, the others are checked in the heap
 *      after the value filters), and
 *   4. adds the surviving tuples to the groups: group keys become dense ids (dictionary codes and integer ranges through
 *      tables built once per dictionary / value range, evaluating the key expression with PostgreSQL's own evaluator on each
 *      distinct value), the ids a group index (direct arrays, a hash table beyond that), and the aggregates accumulate by
 *      group index in tuple order (so float sums do not depend on the SIMD level).
 * The groups are turned into the rows of custom_scan_tlist when the scan has finished; the plan above projects and filters.
 */
#include "postgres.h"

#include <float.h>
#include <math.h>

#include "access/genam.h"
#include "access/heapam.h"
#include "access/htup_details.h"
#include "access/parallel.h"
#include "access/relation.h"
#include "access/table.h"
#include "access/tableam.h"
#include "catalog/pg_aggregate.h"
#include "catalog/pg_operator.h"
#include "catalog/pg_proc.h"
#include "access/skey.h"
#include "access/stratnum.h"
#include "access/xact.h"
#include "catalog/pg_am.h"
#include "catalog/pg_index.h"
#include "catalog/pg_type.h"
#include "commands/explain.h"
#include "common/hashfn.h"
#include "common/int.h"
#include "executor/executor.h"
#include "executor/nodeAgg.h"
#include "executor/nodeHash.h"
#include "executor/nodeSubplan.h"
#include "miscadmin.h"
#include "nodes/extensible.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "optimizer/clauses.h"
#include "optimizer/optimizer.h"
#include "portability/instr_time.h"
#include "parser/parse_agg.h"
#include "storage/predicate.h"
#include "storage/proc.h"
#include "storage/shm_mq.h"
#include "storage/shm_toc.h"
#include "utils/acl.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/date.h"
#include "utils/datum.h"
#include "utils/fmgroids.h"
#include "utils/fmgrprotos.h"
#include "utils/expandeddatum.h"
#include "utils/float.h"
#include "utils/guc.h"
#include "utils/numeric.h"
#include "utils/lsyscache.h"
#include "utils/numeric.h"
#include "utils/rel.h"
#include "utils/snapmgr.h"
#include "utils/syscache.h"
#include "utils/timestamp.h"
#include "utils/tuplesort.h"
#include "utils/wait_event.h"
#include "utils/typcache.h"

#include "agg/agg.h"
#include "agg/wide.h"
#include "agg/back.h"
#include "agg/kern.h"
#include "agg/part.h"
#include "agg/refuse.h"
#include "agg/rows.h"
#include "agg/spec.h"
#include "agg/stats.h"
#include "col/kern.h"
#include "agg/colsrc.h"
#include "agg/search.h"
#include "agg/topn.h"
#include "core/visibility.h"
#include "core/popcount.h"
#include "agg/agg_int.h"

bool		sift_enable_agg = true;
bool		sift_agg_preagg = true;
bool		sift_agg_dense = true;
bool		sift_agg_semi_lut = true;
bool		sift_agg_two_level_on = true;
int		sift_agg_semi_lut_size = 4096;
bool		sift_agg_lookup_pushdown = true;
bool		sift_agg_join_filter = true;
bool		sift_agg_key_skew = true;
bool		sift_agg_share_lookups = true;
bool		sift_agg_fuse = true;
bool		sift_enable_topn = true;
bool		sift_topn_walk = true;
static bool sift_test_agg_wait = false;
int		sift_agg_combo_cache = 1 << 20;

void
sift_agg_init(void)
{
	sift_aggkern_init();
	sift_aggcol_register();
	sift_aggcte_init();
	sift_search_init();
	sift_agg_rows_init();
	sift_agg_refuse_init();
	DefineCustomBoolVariable("sift.enable_agg",
							 "Enables aggregate pushdown into sift_col column indexes (Custom Scan (SiftAgg)).",
							 NULL, &sift_enable_agg, true,
							 PGC_USERSET, 0, NULL, NULL, NULL);
	DefineCustomBoolVariable("sift.agg_preagg",
							 "Answers the units of an aggregate scan whose rows are all visible from the pre-aggregates the sift_col index stored with them.",
							 NULL, &sift_agg_preagg, true,
							 PGC_USERSET, 0, NULL, NULL, NULL);
	DefineCustomBoolVariable("sift.agg_preagg_distinct",
							 "Answers count(DISTINCT col) of a column the cube stores a histogram of from the groups of the histograms of the units whose rows are all visible, with sift.agg_preagg.",
							 NULL, &sift_agg_preagg_distinct, true,
							 PGC_USERSET, 0, NULL, NULL, NULL);
	DefineCustomRealVariable("sift.agg_rollup_cost",
							 "Scale of the cost of a tuple of a unit's rollup against the scan of an element: a unit is answered from its rollup only when that costs less (0: always).",
							 NULL, &sift_agg_rollup_cost, 1.0, 0.0, 1000.0,
							 PGC_USERSET, GUC_NOT_IN_SAMPLE, NULL, NULL, NULL);
	DefineCustomBoolVariable("sift.agg_dense",
							 "Aggregates the chunks of counts, sums and averages from the bitmap of their rows, without a list of tuples.",
							 NULL, &sift_agg_dense, true,
							 PGC_USERSET, GUC_NOT_IN_SAMPLE, NULL, NULL, NULL);
	DefineCustomBoolVariable("sift.agg_semi_lut",
							 "Answers a semi or anti join (IN, EXISTS, NOT EXISTS) on an integer leaf from a byte table over the keys of the joined table.",
							 NULL, &sift_agg_semi_lut, true,
							 PGC_USERSET, 0, NULL, NULL, NULL);
	DefineCustomBoolVariable("sift.agg_two_level",
							 "Aggregates an aggregate over per-row aggregates (UNION ALL arms grouped by a row id) in one scan of the index: the cells of a row are kept in the scan.",
							 NULL, &sift_agg_two_level_on, true,
							 PGC_USERSET, 0, NULL, NULL, NULL);
	DefineCustomBoolVariable("sift.agg_lookup_pushdown",
							 "Loads the rows a lookup join can match only: an index scan or scan keys on the table's own `column = value` conditions, and the other own conditions tested while loading.",
							 NULL, &sift_agg_lookup_pushdown, true,
							 PGC_USERSET, 0, NULL, NULL, NULL);
	DefineCustomBoolVariable("sift.agg_join_filter",
							 "Tests the integer key leaf of an inner or semi lookup join against the keys of the loaded table before tuples are made (zone pruning, source filters, row bitmaps).",
							 NULL, &sift_agg_join_filter, true,
							 PGC_USERSET, 0, NULL, NULL, NULL);
	DefineCustomBoolVariable("sift.agg_key_skew",
							 "Prices the probes of the normal plan for the keys of a lookup join by the rows a key owns by the column's most common values, not by the mean rows of a key.",
							 NULL, &sift_agg_key_skew, true,
							 PGC_USERSET, 0, NULL, NULL, NULL);
	DefineCustomBoolVariable("sift.agg_share_lookups",
							 "Lets the Custom Scan (SiftAgg) nodes of one statement that join the same table share the rows they load (UNION ALL arms).",
							 NULL, &sift_agg_share_lookups, true,
							 PGC_USERSET, 0, NULL, NULL, NULL);
	DefineCustomBoolVariable("sift.agg_fuse",
							 "Runs the UNION ALL arms that read the same rows in one pass over the index, as the grouping sets of one scan.",
							 NULL, &sift_agg_fuse, true,
							 PGC_USERSET, 0, NULL, NULL, NULL);
	DefineCustomIntVariable("sift.agg_lookup_cache_kb",
							"Memory per backend that keeps the rows of the joined tables a scan loaded for the next query, while the table does not change (0: off).",
							NULL, &sift_agg_lookup_cache_kb, 256 * 1024, 0, MAX_KILOBYTES,
							PGC_USERSET, GUC_UNIT_KB, NULL, NULL, NULL);
	DefineCustomIntVariable("sift.agg_lookup_shared_kb",
							"Shared memory, for all backends together, that keeps the rows of the joined tables a scan loaded for the queries of other backends, while the table does not change (0: off). Dynamic shared memory, taken as entries are kept and returned as they go.",
							NULL, &sift_agg_lookup_shared_kb, 0, 0, MAX_KILOBYTES,
							PGC_SUSET, GUC_UNIT_KB, NULL, NULL, NULL);
	DefineCustomIntVariable("sift.agg_semi_lut_size",
							"Memory of the byte table of a semi or anti join, per scan and per worker; keys that span more keep the generic join.",
							NULL, &sift_agg_semi_lut_size, 4096, 0, 1024 * 1024,
							PGC_USERSET, GUC_UNIT_KB, NULL, NULL, NULL);
	DefineCustomBoolVariable("sift.enable_topn",
							 "Enables ORDER BY .. LIMIT from the column units (Custom Scan (SiftAgg) with Top-N), with sift.enable_agg.",
							 NULL, &sift_enable_topn, true,
							 PGC_USERSET, 0, NULL, NULL, NULL);
	DefineCustomBoolVariable("sift.topn_walk",
							 "Lets a top-N scan visit the units of a sort column that does not follow the table by the best value of their zones, and stop at the first that cannot beat the n-th.",
							 NULL, &sift_topn_walk, true,
							 PGC_USERSET, 0, NULL, NULL, NULL);
	DefineCustomBoolVariable("sift.topn_zone_keys",
							 "Lets the walk of a top-N bound the units by the zones of the sort keys after the first too: a first key that ties between units is decided by the next.",
							 NULL, &sift_topn_zone_keys, true,
							 PGC_USERSET, 0, NULL, NULL, NULL);
	DefineCustomIntVariable("sift.agg_param_set_size",
							"Elements assumed for an array parameter of a set filter (col = ANY($1)) in a generic plan, to cost the index and bitmap scans against Custom Scan (SiftAgg); 0 keeps the planner's own guess.",
							NULL, &sift_agg_param_set_size, 1000, 0, 100000000,
							PGC_USERSET, 0, NULL, NULL, NULL);
	DefineCustomIntVariable("sift.agg_lookup_max_rows",
							"Rows a sub-select, VALUES list or function joined to Custom Scan (SiftAgg) may return, as the planner estimates them: the rows go into a lookup table in memory when the scan starts; a larger one keeps the normal join.",
							NULL, &sift_agg_lookup_max_rows, 1000000, 0, INT_MAX,
							PGC_USERSET, 0, NULL, NULL, NULL);
	DefineCustomBoolVariable("sift.test_agg_wait",
							 "Testing: a top-N scan waits, once its source is open and before it reads a unit, for the advisory lock (20983, 1) to be free.",
							 NULL, &sift_test_agg_wait, false,
							 PGC_USERSET, GUC_NOT_IN_SAMPLE, NULL, NULL, NULL);
	sift_aggbe_init();
	DefineCustomIntVariable("sift.agg_combo_cache",
							"Distinct value combinations of an expression PostgreSQL evaluates for the aggregate scan kept before the cache starts again.",
							NULL, &sift_agg_combo_cache, 1 << 20, 16, 1 << 24,
							PGC_USERSET, GUC_NOT_IN_SAMPLE, NULL, NULL, NULL);
}

static const char null_dict_id = 0;
static const uint32 null_dict_off[1] = {0};
static const SiftSrcDict null_dict = {&null_dict_id, 0, null_dict_off, ""};
static const uint64 null_data[SIFT_AGG_PAD] pg_attribute_aligned(SIFT_SIMD_ALIGN);
static const uint8 null_flags[SIFT_AGG_PAD] = {[0 ... SIFT_AGG_PAD - 1] = 1};

/* a unit of one row whose leaves are all NULL: what a row of the preserved side of an outer join joins to when no row of the scan matched it */
static void
null_unit(AggExec *ex)
{
	ex->null_input = true;
	MemoryContextReset(ex->ucxt);
	memset(ex->gofs, 0, ex->lay->nleaves * sizeof(uint32 *));
	memset(ex->gpar, 0, ex->lay->nleaves * sizeof(uint32 *));
	for (int i = 0; i < ex->spec->npreds_total; i++)
	{
		ex->ps[i].rowmask_ok = false;
		ex->ps[i].poison_ok = false;
	}
	for (int leaf = 0; leaf < ex->lay->nleaves; leaf++)
		ex->uvec[leaf] = (SiftSrcVec) {null_data, null_flags, &null_dict, 1, NULL};
	memset(ex->uvec_ok, true, ex->lay->nleaves * sizeof(bool));
	ex->unit.nrow = 1;
	ex->rsel = NULL;
	ex->qlevel = 0;
	ex->cstart = 0;
	ex->cn = 1;
}

/* Rows the nullable input never matched: every earlier lookup column is NULL as well. */
static void
unmatched_tail(AggExec *ex)
{
	int			ci;
	Tup		   *own;
	ComboEnt	ent;
	uint32		prior[SIFT_AGG_MAX_COMBOS] = {0};

	if (ex->nj == 0 || !ex->cs[ex->jcombo[ex->nj - 1]].map->keep)
		return;
	ci = ex->jcombo[ex->nj - 1];
	own = &ex->stage[ex->nj - 1];
	null_unit(ex);
	for (int x = 0; x < ex->nj - 1; x++)
	{
		int			prev = ex->jcombo[x];
		const SiftAggCombo *cb = &ex->spec->combo[prev];
		uint64		v[SIFT_AGG_MAX_CLEAVES] = {0};
		ComboEnt	null;
		MemoryContext old = MemoryContextSwitchTo(ex->comcxt);

		for (int p = 0; p < cb->nleaves; p++)
			if (cb->leaf[p] < 0)
				v[p] = (uint64) prior[cb->refcombo[p]] + 1;
		combo_bind_leaves(ex, prev, v);
		combo_emit(ex, prev, &null, NULL, 0, 1);
		prior[prev] = null.base;
		MemoryContextSwitchTo(old);
		MemoryContextReset(ex->comcxt);
	}
	ent = combo_unmatched(ex, ci, prior);
	for (uint32 e = 0; e < ent.nout;)
	{
		int			k = 0;

		for (; k < VEC && e < ent.nout; k++)
		{
			own->sel[k] = own->tidx[k] = own->ridx[k] = 0;
			for (int x = 0; x < ex->nj - 1; x++)
				own->cout[ex->jcombo[x]][k] = prior[ex->jcombo[x]];
			own->cout[ci][k] = ent.base + e++;
		}
		own->n = k;
		agg_tuples(ex, own);
	}
	if (ex->lane_serial)
		lane_fold(ex);
}

void
run_scan(AggExec *ex)
{
	bool		ordered = sift_aggspec_ordered(ex->spec),
				none;

	ex->null_input = false;
	ex->started = true;
	ex->glist = NULL;
	engine_open(ex, ex->snap, ex->params);
	scan_buffers(ex);
	none = join_none(ex);
	if (!backend_begin(ex) && !none)
		par_launch(ex);
	if (ex->shared == NULL)
	{
		ex->shared = MemoryContextAlloc(ex->cxt, shared_size(ex->spec, 0));
		shared_init(ex->shared, ex);
	}
	/* a join without a row to match leaves no tuple: no item of work to take */
	if (none)
		pg_atomic_write_u32(&ex->shared->next, ex->shared->nitems);
	ex->lane_serial = ordered && ex->par == NULL;
	ex->lanes = ordered && !ex->lane_serial;
	table_init(ex);
	if (ex->spec->spine)
		spine_load(ex);
	ex->gtab = ex->lanes ? result_view(ex) : ex;
	ex->pending = MemoryContextAllocZero(ex->cxt, SIFT_AGG_LANES * sizeof(char *));
	if (ex->par == NULL || parallel_leader_participation)
		scan_items(ex);
	backend_finish(ex);
	unmatched_tail(ex);
	arms_flush(ex);
	if (ex->spec->spine)
		spine_tail(ex);
	if (ex->par)
		par_finish(ex);
	for (int i = 0; ex->lane_serial && i < ex->spec->naggs; i++)
		if (ex->acc[i].dt)
			memcpy(ex->acc[i].d, ex->acc[i].dt, (ex->hash ? ex->ngroups : ex->gcap) * sizeof(double));
	osa_finish_all(ex->gtab);
	gen_flush(ex->gtab);
	if (ex->lanes && ex->next_lane != SIFT_AGG_LANES)
		elog(ERROR, "sift: a lane of the aggregate scan is missing");
	final_checks(ex->gtab, ex->n_tuples);
}

/* the groups that exist, in index order */
void
list_groups(AggExec *ex)
{
	uint32		n = ex->hash ? ex->ngroups : ex->gcap,
				k = 0;

	ex->glist = MemoryContextAlloc(ex->cxt, Max(n, 1) * sizeof(uint32));
	for (uint32 g = 0; g < n; g++)
		if (ex->seen[g] > 0)
			ex->glist[k++] = g;
	if (ex->spec->nkeys == ex->spec->nhidden && k == 0)
		ex->glist[k++] = 0;
	ex->nglist = k;
	ex->n_groups = k;
}

Datum
key_datum(AggExec *ex, int j, uint32 g, bool *isnull)
{
	return key_id_datum(ex, j, key_id(ex, j, g), isnull);
}

/* the value of id `id` of key j */
Datum
key_id_datum(AggExec *ex, int j, uint32 id, bool *isnull)
{
	KeyState   *k = &ex->ks[j];

	*isnull = id == 0;
	if (id == 0)
		return (Datum) 0;
	if (k->numk)
		return ex->spec->key[j].typid == NUMERICOID ? scaled_numeric(k->im.val[id - 1], ex->spec->key[j].num.scale) :
			leaf_datum(ex->spec->key[j].typid, (int64) k->im.val[id - 1]);
	if (k->ident)
		/* PG_INT64_MIN: the bits of -0 */
		return id == k->negzero ? leaf_datum(ex->spec->key[j].typid, PG_INT64_MIN) : leaf_datum(ex->spec->key[j].typid, k->idval[id - 1]);
	return k->ot.val[id - 1];
}

Datum
agg_datum(AggExec *ex, int ai, uint32 g, bool *isnull)
{
	SiftAggAgg *a = &ex->spec->agg[ai];
	AggAcc	   *acc = &ex->acc[ai];

	*isnull = false;
	switch (a->kind)
	{
			case SAA_COUNT_STAR:
		case SAA_COUNT:
		case SAA_COUNT_ROWS:
		case SAA_COUNT_DISTINCT:
		case SAA_O_COUNT_STAR:
		case SAA_O_COUNT_ROWS:
		case SAA_O_COUNT_KEY:
		case SAA_M_COUNT:
		case SAA_M_DISTINCT:
		case SAA_X_COUNT:
			return Int64GetDatum(acc->n[g]);
		case SAA_GROUPING:
			return Int32GetDatum(grouping_bits(ex, a, g));
		case SAA_M_MIN:
		case SAA_M_MAX:
		case SAA_X_MIN:
		case SAA_X_MAX:
			*isnull = acc->n[g] == 0;
			return acc->v[g];
		case SAA_M_SUM:
		case SAA_M_AVG:
		case SAA_X_SUM:
		case SAA_X_AVG:
			return mapped_sum_datum(ex, a, ai, g, isnull);
		case SAA_GEN:
			*isnull = (acc->nf[g] & GEN_RNULL) != 0;
			return *isnull ? (Datum) 0 : acc->r[g];
		case SAA_M_PG:
			*isnull = acc->n[g] == 0 || !acc->nf[g];
			return *isnull ? (Datum) 0 : acc->v[g];
		case SAA_O_SUM_I64:
			*isnull = acc->n[g] == 0;
			return DirectFunctionCall1(int8_numeric, Int64GetDatum(acc->i[g]));
		case SAA_O_SUM_F64:
			*isnull = acc->n[g] == 0;
			if (isinf(acc->d[g]) && !acc->nf[g])
				float_overflow_error();
			return Float8GetDatum(acc->d[g]);
		case SAA_O_AVG_F64:
			*isnull = acc->n[g] == 0;
			return Float8GetDatum(acc->d[g] / (double) acc->n[g]);
		case SAA_SUM_INT:
			*isnull = acc->n[g] == 0;
			return Int64GetDatum(acc->i[g]);
		case SAA_SUM_F64:
			*isnull = acc->n[g] == 0;
			if (isinf(acc->d[g]) && !acc->nf[g])
				float_overflow_error();
			return Float8GetDatum(acc->d[g]);
		case SAA_AVG_INT:
			if (acc->n[g] == 0)
			{
				*isnull = true;
				return (Datum) 0;
			}
			return DirectFunctionCall2(numeric_div, DirectFunctionCall1(int8_numeric, Int64GetDatum(acc->i[g])),
									   DirectFunctionCall1(int8_numeric, Int64GetDatum(acc->n[g])));
		case SAA_AVG_F64:
			*isnull = acc->n[g] == 0;
			return Float8GetDatum(acc->d[g] / (double) acc->n[g]);
		case SAA_SUM_NUM:
			*isnull = acc->n[g] == 0;
			return *isnull ? (Datum) 0 : scaled_numeric(acc->nf[g] ? SIFT_NX_NAN : acc->w[g], a->num.scale);
		case SAA_AVG_NUM:
			*isnull = acc->n[g] == 0;
			if (*isnull)
				return (Datum) 0;
			return DirectFunctionCall2(numeric_div, scaled_numeric(acc->nf[g] ? SIFT_NX_NAN : acc->w[g], a->num.scale),
									   DirectFunctionCall1(int8_numeric, Int64GetDatum(acc->n[g])));
		case SAA_PCT_CONT:
		case SAA_PCT_DISC:
			{
				double		v;

				*isnull = acc->n[g] == 0;
				if (*isnull)
					return (Datum) 0;
				v = ex->oval[ai][ex->acc[a->share].i[g] - 1];
				if (a->restype == FLOAT4OID)
					return Float4GetDatum((float4) v);
				return a->restype == FLOAT8OID ? Float8GetDatum(v) : leaf_datum(a->restype, (int64) v);
			}
		case SAA_MIN:
		case SAA_MAX:
			*isnull = acc->n[g] == 0;
			if (!a->num.n && ex->lay->leaf[a->leaf].vtype == SCV_F64)
				return a->restype == FLOAT4OID ? Float4GetDatum((float4) acc->d[g]) : Float8GetDatum(acc->d[g]);
			if (a->num.n)
				return scaled_numeric(acc->w[g], a->num.scale);
			return leaf_datum(a->restype, acc->i[g]);
	}
	return (Datum) 0;
}

/* The zone of the first sort leaf in the unit of superblock sb; false when it has none */
static bool
topn_zone(AggExec *ex, uint32 sb, SiftSrcZone *z)
{
	if (ex->search)
		sift_colsrc_select(ex->src, NULL);	/* the units as they are, not those of the last match set */
	sift_colsrc_begin(ex->src, sb, sb + 1, NULL, 0);
	if (!sift_colsrc_next(ex->src, &ex->unit))
		return false;
	sift_colsrc_zone(ex->src, &ex->unit, sift_topn_leaf(ex->topn, 0), z);
	return true;
}

/*
 * Does a unit of the superblocks [lo, hi) have a zone that can hold a better row than the n-th so far? The new version of an updated row is
 * stored wherever there is room (a page with free space in a heap, the write buffer of a table) whatever the value of the sort column, so
 * the superblocks do not follow the column even when the zones of a few of them say so, and no superblock can be taken for the last that
 * matters: every window is decided by the zones of its own units (read together from the segment headers, no unit is opened), and the match
 * set is built only for a window that has one.
 */
static bool
topn_window_viable(AggExec *ex, uint32 lo, uint32 hi)
{
	SiftSrcZone z;
	uint32		n = sift_colsrc_zone_range(ex->src, lo, hi, sift_topn_leaf(ex->topn, 0), &z);

	if (sift_topn_zone_viable(ex->topn, &z))
		return true;
	while (n-- > 0)
		sift_topn_skip(ex->topn);
	return false;
}

/* Windows of superblocks from the end the best values are at (agg/topn.h) */

/*
 * The match set of the superblocks a window is in. The first window is asked for alone (the scan often ends in it, and a match set costs by
 * the superblocks it covers); the chunk around each later one holds the windows that follow it, which a backwards visit cannot get cheaply:
 * the match iterator restarts when asked for superblocks it has passed.
 */
typedef struct TopnChunk
{
	uint32		lo,
				hi;
	const SiftFilter *flt;
} TopnChunk;

static void
topn_scan(AggExec *ex, uint32 lo, uint32 hi, uint32 nsb, TopnChunk *ch)
{
	if (ex->search)
	{
		if (ch->flt == NULL || lo < ch->lo || hi > ch->hi)
		{
			uint32		width = ch->flt == NULL ? hi - lo : Max(hi - lo, sift_search_chunk_sbs);

			ch->lo = sift_topn_reverse(ex->topn) ? (hi > width ? hi - width : 0) : lo;
			ch->hi = sift_topn_reverse(ex->topn) ? hi : Min(lo + width, nsb);
			ch->flt = sift_search_range(ex->search, ch->lo, ch->hi);
		}
		sift_colsrc_select(ex->src, ch->flt);
	}
	scan_units(ex, lo, hi);
}

/* Testing (sift.test_agg_wait): lets a test run VACUUM or DML after the scan has opened its source (test/t/topn_vacuum_scan.sh) */
static void
agg_test_wait(void)
{
	LOCKTAG		tag;

	if (!sift_test_agg_wait)
		return;
	SET_LOCKTAG_ADVISORY(tag, MyDatabaseId, 0x51F7, 1, 2);
	(void) LockAcquire(&tag, ShareLock, false, false);
	LockRelease(&tag, ShareLock, false);
}

/*
 * The items of a parallel top-N, taken from the shared counter by the leader and the workers: a window of superblocks of the leader's visit
 * order, left out when the zones of its units cannot beat the n-th row of any participant (the bound they share, agg/topn.h)
 */
void
topn_items(AggExec *ex)
{
	ScanShared *sh = ex->shared;
	TopnChunk	ch = {0, 0, NULL};
	uint32		item;

	while (take_item(ex, &item))
	{
		uint32		lo,
					hi;

		if (sh->topn_walk)
		{
			lo = shared_order(sh, ex->spec)[item];
			hi = lo + 1;
		}
		else if (sh->topn_reverse)
		{
			hi = sh->nsb - item * sh->topn_item;
			lo = hi > sh->topn_item ? hi - sh->topn_item : 0;
		}
		else
		{
			lo = item * sh->topn_item;
			hi = Min(lo + sh->topn_item, sh->nsb);
		}
		if (sift_topn_wants_zones(ex->topn) && !topn_window_viable(ex, lo, hi))
		{
			/* the walk gives the best bound first: the superblocks after this one are no better, whatever bound the others reach */
			if (sh->topn_walk)
			{
				pg_atomic_write_u32(&sh->next, sh->nitems);
				break;
			}
			continue;
		}
		topn_scan(ex, lo, hi, sh->nsb, &ch);
	}
}

/* The workers and the leader take the items; the heaps of the workers go into the leader's, which is the result as one process finds it */
static void
topn_parallel(AggExec *ex)
{
	par_launch(ex);
	if (ex->shared == NULL)
	{
		ex->shared = MemoryContextAlloc(ex->cxt, shared_size(ex->spec, ex->nvorder));
		shared_init(ex->shared, ex);
	}
	sift_topn_share(ex->topn, &ex->shared->topn_bound);
	if (ex->par == NULL || parallel_leader_participation)
		topn_items(ex);
	if (ex->par)
		par_finish(ex);
	sift_topn_finish(ex->topn);
}

static void
topn_run(AggExec *ex)
{
	SiftTopN   *t;
	uint32		nsb,
				lo,
				hi,
				best;
	SiftSrcZone z;
	TopnChunk	ch = {0, 0, NULL};
	bool		parallel;

	ex->started = true;
	engine_open(ex, ex->snap, ex->params);
	agg_test_wait();
	scan_buffers(ex);
	table_init(ex);
	t = ex->topn;
	if (sift_topn_empty(t))
		return;
	nsb = sift_colsrc_nsb(ex->src);
	if (sift_topn_wants_zones(t) && nsb >= 2 * sift_topn_nprobes())
		for (int i = 0; i < sift_topn_nprobes(); i++)
			if (topn_zone(ex, sift_topn_probe_sb(t, i, nsb), &z))
				sift_topn_probe(t, ex->unit.sb, &z);
	sift_topn_start(t, nsb);
	parallel = ex->spec->nworkers > 0 && sift_topn_parallel_ok(t);
	if (sift_topn_walk && ex->search == NULL && sift_topn_wants_zones(t) && !sift_topn_follows(t) && nsb >= 2 * sift_topn_nprobes())
	{
		/* the match set of a search restarts when it is asked for superblocks it has passed: only the scans without one are walked */
		sift_topn_walk_begin(t, nsb);
		for (uint32 sb = 0; sb < nsb; sb++)
		{
			SiftSrcZone zs[SIFT_TOPN_MAX_KEYS];
			uint32		units = 0;

			for (int j = 0; j < sift_topn_walk_keys(t); j++)
				units = sift_colsrc_zone_range(ex->src, sb, sb + 1, sift_topn_leaf(t, j), &zs[j]);
			sift_topn_walk_add(t, sb, units, zs);
		}
		sift_topn_walk_sort(t);
		if (parallel)
		{
			ex->vorder = (uint32 *) sift_topn_walk_order(t);
			ex->nvorder = nsb;
			topn_parallel(ex);
			return;
		}
		while (sift_topn_walk_next(t, &lo))
			scan_units(ex, lo, lo + 1);
		sift_topn_finish(t);
		return;
	}
	if (parallel)
	{
		topn_parallel(ex);
		return;
	}
	while (sift_topn_next_window(t, &lo, &hi, &best))
	{
		if (sift_topn_wants_zones(t) && !topn_window_viable(ex, lo, hi))
			continue;
		topn_scan(ex, lo, hi, nsb, &ch);
	}
	sift_topn_finish(t);
}

static TupleTableSlot *
topn_next(AggExec *ex, TupleTableSlot *slot)
{
	if (!ex->started)
		topn_run(ex);
	return !sift_topn_empty(ex->topn) && sift_topn_next(ex->topn, slot) ? slot : NULL;
}

static TupleTableSlot *
agg_next(ScanState *node)
{
	AggExec    *ex = (AggExec *) node;
	TupleTableSlot *slot = node->ss_ScanTupleSlot;
	AggExec    *t;
	uint32		g;

	if (ex->spec->topn)
		return topn_next(ex, slot);
	if (!ex->started && !fuse_start(ex))
	{
		run_scan(ex);
		empty_set_groups(ex->gtab);
		dist_finish_all(ex->gtab);
		dist_end_all(ex->gtab);
		mapped_finish(ex->gtab);
		list_groups(ex->gtab);
		gen_finish(ex->gtab);
		ex->n_groups = ex->gtab->nglist;
	}
	if (ex->fuse)
		return fuse_next(ex, slot);
	t = ex->gtab;
	if (ex->gpos >= t->nglist)
		return NULL;
	g = t->glist[ex->gpos++];
	ExecClearTuple(slot);
	for (int j = 0; j < t->spec->nkeys; j++)
		slot->tts_values[j] = key_datum(t, j, g, &slot->tts_isnull[j]);
	for (int i = 0; i < t->spec->naggs; i++)
	{
		int			c = t->spec->nkeys + i;

		slot->tts_values[c] = agg_datum(t, i, g, &slot->tts_isnull[c]);
	}
	return ExecStoreVirtualTuple(slot);
}

static bool
agg_recheck(ScanState *node, TupleTableSlot *slot)
{
	return true;
}

void
engine_contexts(AggExec *ex, MemoryContext parent)
{
	ex->cxt = AllocSetContextCreate(parent, "sift agg", ALLOCSET_DEFAULT_SIZES);
	ex->tcxt = AllocSetContextCreate(ex->cxt, "sift agg table", ALLOCSET_DEFAULT_SIZES);
	ex->ucxt = AllocSetContextCreate(ex->cxt, "sift agg unit", ALLOCSET_DEFAULT_SIZES);
}

void
engine_rels(AggExec *ex)
{
	if (OidIsValid(ex->spec->indexoid))
		ex->index = index_open(ex->spec->indexoid, AccessShareLock);
	ex->heap = table_open(ex->spec->heapoid, AccessShareLock);
	ex->heap_opened = true;
}

/* The scan engine on the relations of the spec: shared by the node and by a parallel worker */
void
engine_open(AggExec *ex, Snapshot snapshot, ParamListInfo params)
{
	MemoryContext old = MemoryContextSwitchTo(ex->cxt);

	ex->src = ex->spec->nparts > 0 ? sift_aggpart_open(ex->heap, ex->spec->nparts, ex->spec->partheap, ex->spec->partindex, snapshot, ex->cxt) :
		sift_colsrc_open(ex->heap, ex->index, snapshot, ex->cxt);
	if (ex->partoff)
		sift_aggpart_set_offsets(ex->src, ex->partoff);
	ex->lay = sift_colsrc_source_layout(ex->src);
	extend_layout(ex);
	ex->uvec = palloc0(ex->lay->nleaves * sizeof(SiftSrcVec));
	ex->uvec_ok = palloc0(ex->lay->nleaves * sizeof(bool));
	ex->gofs = palloc0(ex->lay->nleaves * sizeof(uint32 *));
	ex->gpar = palloc0(ex->lay->nleaves * sizeof(uint32 *));
	/* its per-query memory is where a sub-plan runs its scans (init_lut_expr) */
	MemoryContextSwitchTo(ex->css.ss.ps.state ? ex->css.ss.ps.state->es_query_cxt : ex->cxt);
	ex->lutcx = CreateStandaloneExprContext();
	MemoryContextSwitchTo(ex->cxt);
	ex->lutcx->ecxt_param_list_info = params;
	ex->lutcx->ecxt_param_exec_vals = ex->lutparams;
	part_prune_init(ex);
	if (ex->spec->search)
		ex->search = sift_search_open(ex->spec->search, ex->heap, snapshot, ex->lutcx, EXPR_PARENT(ex), ex->cxt);
	if (ex->spec->topn)
		ex->topn = sift_topn_open(ex->spec->topn, ex->lay, ex->heap, snapshot, ex->lutcx, EXPR_PARENT(ex), ex->cxt);
	init_states(ex);
	register_preds(ex);
	for (int j = 0; j < ex->spec->nkeys; j++)
		register_key(ex, j);
	register_combos(ex);
	join_filters(ex, old);
	pgagg_init(ex);
	prune_init(ex);
	filter_init(ex);
	type_init(ex);
	recheck_init(ex);
	wide_init(ex);
	tid_init(ex, snapshot);
	str_init(ex);
	preagg_init(ex);
	dense_init(ex);
	gen_open(ex);
	MemoryContextSwitchTo(old);
}

void
engine_close(AggExec *ex)
{
	if (ex->search)
		sift_search_close(ex->search);
	if (ex->topn)
		sift_topn_close(ex->topn);
	if (ex->tid_vis)
	{
		sift_vis_end(ex->tid_vis);
		ExecDropSingleTupleTableSlot(ex->tid_slot);
	}
	if (ex->src)
		sift_colsrc_close(ex->src);
	if (ex->lutcx)
		FreeExprContext(ex->lutcx, true);
	if (ex->pgstate)
		FreeExprContext(ex->pgstate->curaggcontext, true);
	if (ex->index)
		index_close(ex->index, NoLock);
	if (ex->heap_opened)
		table_close(ex->heap, NoLock);
}

static void
agg_begin(CustomScanState *node, EState *estate, int eflags)
{
	AggExec    *ex = (AggExec *) node;
	CustomScan *cs = (CustomScan *) node->ss.ps.plan;

	ex->spec = sift_aggspec_unpack(cs->custom_private);
	ex->pack = cs->custom_private;
	ex->params = estate->es_param_list_info;
	ex->snap = estate->es_snapshot;
	engine_contexts(ex, estate->es_query_cxt);
	/* the child plans of the joins (a sub-select, VALUES list or function) are the provider's to start and end; the scan reads each once */
	foreach_ptr(Plan, child, cs->custom_plans)
		node->custom_ps = lappend(node->custom_ps, ExecInitNode(child, estate, eflags & ~(EXEC_FLAG_BACKWARD | EXEC_FLAG_MARK)));
	if (eflags & EXEC_FLAG_EXPLAIN_ONLY)
		return;
	engine_rels(ex);
	PredicateLockRelation(ex->heap, estate->es_snapshot);
	fuse_register(ex);
}

static TupleTableSlot *
agg_exec(CustomScanState *node)
{
	return ExecScan(&node->ss, agg_next, agg_recheck);
}

/* what the source counted for the run, with the workers' */
static void
run_counters(AggExec *c, SiftSrcCounters *cc)
{
	sift_colsrc_counters(c->src, cc);
	if (c->par)
	{
		cc->units_pruned += c->par->cc.units_pruned;
		cc->rows_vetted += c->par->cc.rows_vetted;
		cc->heap_pages += c->par->cc.heap_pages;
		cc->rechecked += c->par->cc.rechecked;
		cc->recheck_units += c->par->cc.recheck_units;
	}
}

/*
 * Folds the run into the counters of the backend (sift.stats) when it ends, from the counters the loops keep for EXPLAIN ANALYZE: the loops
 * count nothing more. A scan whose pre-aggregates did not answer it is a miss of its index, for the reason pa_why gives (or, when they were on,
 * for the units that lacked their rollup).
 */
static void
stats_run(AggExec *c)
{
	SiftSrcCounters cc;
	Oid			rel = OidIsValid(c->spec->indexoid) ? c->spec->indexoid : c->spec->heapoid;

	run_counters(c, &cc);
	sift_stat_add(SIFT_STAT_SCANS, 1);
	sift_stat_add(SIFT_STAT_WORKERS, c->par ? c->par->nlaunched : 0);
	sift_stat_add(SIFT_STAT_UNITS, c->n_units);
	sift_stat_add(SIFT_STAT_UNITS_PRUNED, cc.units_pruned);
	sift_stat_add(SIFT_STAT_UNITS_PREAGG, c->n_pre_units);
	sift_stat_add(SIFT_STAT_ROWS_DECODED, c->n_rows);
	sift_stat_add(SIFT_STAT_ROWS_VETTED, cc.rows_vetted);
	sift_stat_add(SIFT_STAT_ROWS_RECHECKED, cc.rechecked);
	sift_stat_add(SIFT_STAT_UNITS_RECHECKED, cc.recheck_units);
	sift_stat_add(SIFT_STAT_UNITS_NO_ROLLUP, c->n_rollup_miss);
	sift_stat_add(SIFT_STAT_UNITS_ROLLUP_COST, c->n_rollup_cost);
	if (c->pa_why != SPW_OFF && c->pa_why != SPW_OK)
		sift_stats_preagg_miss(rel, sift_pawhy_slug(c->pa_why), sift_pawhy_fixable(c->pa_why), 0);
	else if (c->n_rollup_miss > 0)
		sift_stats_preagg_miss(rel, "unit_without_rollup", true, c->n_rollup_miss);
}

static void
agg_end(CustomScanState *node)
{
	AggExec    *ex = (AggExec *) node;

	for (int i = 0; ex->osa && i < ex->spec->naggs; i++)
		if (ex->osa[i] && ex->spec->agg[i].share == i && ex->osa[i]->sort)
			tuplesort_end(ex->osa[i]->sort);
	gen_close(ex);
	if (ex->css.ss.ps.state)
		fuse_unregister(ex);
	if (ex->fuse && --ex->fuse->refs == 0)
	{
		stats_run(ex->fuse->F);
		engine_close(ex->fuse->F);
	}
	dist_end_all(ex);
	if (ex->gtab)
		dist_end_all(ex->gtab);
	if (ex->started && !ex->fuse)
		stats_run(ex);
	engine_close(ex);
	if (ex->cxt)
		MemoryContextDelete(ex->cxt);
	foreach_ptr(PlanState, child, node->custom_ps)
		ExecEndNode(child);
}

static void
agg_rescan(CustomScanState *node)
{
	AggExec    *ex = (AggExec *) node;

	/* the plan has no parameter the scan reads (unsupported_clause), so the groups are the same: replay them */
	ex->gpos = 0;
	if (ex->topn)
		sift_topn_rewind(ex->topn);
}

/* the dims and measures of the rollup that answers the scan */
static void
explain_pa_leaves(AggExec *ex, ExplainState *es)
{
	StringInfoData d,
				m;

	initStringInfo(&d);
	initStringInfo(&m);
	for (int i = 0; i < ex->pa.ndims; i++)
		appendStringInfo(&d, "%s%s", i ? ", " : "", ex->lay->leaf[ex->pa.rdim[i]].name);
	for (int i = 0; i < ex->pa.nmeas; i++)
		appendStringInfo(&m, "%s%s", i ? ", " : "", ex->lay->leaf[ex->pa.rmeas[i]].name);
	if (d.len > 0)
		ExplainPropertyText("Pre-aggregate Dims", d.data, es);
	if (m.len > 0)
		ExplainPropertyText("Pre-aggregate Measures", m.data, es);
	pfree(d.data);
	pfree(m.data);
}

/* what the scan gives a correlated plan, as the output of an InitPlan: the deparser asserts on a Param that nothing generates */
static SubPlan *
lookup_inputs(List *params)
{
	SubPlan    *sp = makeNode(SubPlan);

	sift_initplan_named(sp, "input");
	sp->setParam = params;
	return sp;
}

static void
explain_source(SiftAggSpec *s, ExplainState *es)
{
	ExplainPropertyText("Index", s->nparts > 0 ? "(partitions)" : s->indexoid ? get_rel_name(s->indexoid) : "(table)", es);
	if (s->nparts > 0)
	{
		List	   *names = NIL;

		for (int i = 0; i < s->nparts; i++)
			names = lappend(names, get_rel_name(s->partheap[i]));
		ExplainPropertyList("Partitions", names, es);
	}
	if (s->desc_quals && s->desc_quals[0])
		ExplainPropertyText("Filters", s->desc_quals, es);
	if (s->search)
		ExplainPropertyText("Search", sift_search_describe(s->search), es);
	if (s->topn)
		ExplainPropertyText("Top-N", psprintf("%s (limit %s)", s->desc_topn, sift_topn_limit_text(s->topn)), es);
}

static void
explain_keys(SiftAggSpec *s, ExplainState *es)
{
	StringInfoData buf;
	int			h1 = s->nkeys - (s->nsets > 0),	/* the hidden keys are between the outer ones and the set's */
				h0 = h1 - s->nhidden;

	initStringInfo(&buf);
	for (int j = 0; j < s->nkeys; j++)
		if (j < h0 || j >= h1)
			appendStringInfo(&buf, "%s%s", buf.len ? ", " : "", s->key[j].desc);
	if (buf.len)
		ExplainPropertyText("Group Keys", buf.data, es);
	resetStringInfo(&buf);
	for (int j = h0; j < h1; j++)
		appendStringInfo(&buf, "%s%s", buf.len ? ", " : "", s->key[j].desc);
	if (s->nhidden)
		ExplainPropertyText("Cell Keys", buf.data, es);
	resetStringInfo(&buf);
	for (int i = 0; i < s->naggs; i++)
		appendStringInfo(&buf, "%s%s", i ? ", " : "", s->agg[i].desc);
	if (s->naggs)
		ExplainPropertyText("Pushed Aggregates", buf.data, es);
	if (s->group >= 0)
		ExplainPropertyText("Unnests", "yes", es);
	if (s->njoin)
	{
		resetStringInfo(&buf);
		for (int i = 0; i < s->njoin; i++)
			appendStringInfo(&buf, "%s%s", i ? ", " : "", s->join[i].desc);
		ExplainPropertyText("Lookup Joins", buf.data, es);
	}
}

/* The plan of the lookup i that PostgreSQL plans per scanned row, nested in the scan's */
static void
explain_correlated(CustomScanState *node, int i, ExplainState *es)
{
	SiftAggSpec *s = ((AggExec *) node)->spec;
	EState     *estate = node->ss.ps.state;
	QueryDesc   query = {0};
	PlannedStmt stmt = *estate->es_plannedstmt;
	ExplainState child;
	List       *own;
	bool        correlated = s->join[i].params != NIL;
	char       *group = s->join[i].first.ok && s->join[i].first.use ? "First Row Lookup" : correlated ? "Correlated Lookup" : "Lookup Plan";
	char       *label = psprintf("%s %d", group, i + 1);

	ExplainOpenGroup(group, label, true, es);
	ExplainPropertyText("Name", s->join[i].desc, es);
	/* the statement's identifier and settings are printed once, at the top */
	stmt.queryId = UINT64CONST(0);
	query.plannedstmt = &stmt;
	query.planstate = list_nth(estate->es_subplanstates, s->join[i].subplan - 1);
	query.estate = estate;
	child = *es;
	child.settings = false;
	child.printed_subplans = NULL;
	own = query.planstate->plan->initPlan;
	if (correlated || s->join[i].first.params != NIL)
		query.planstate->plan->initPlan = lcons(lookup_inputs(correlated ? s->join[i].params : s->join[i].first.params), own);
	ExplainPrintPlan(&child, &query);
	query.planstate->plan->initPlan = own;
	es->indent = child.indent;
	es->grouping_stack = child.grouping_stack;
	ExplainCloseGroup(group, label, true, es);
}

/* The source's counters of the filters and lookup tables a run used */
static void
explain_source_run(AggExec *c, SiftAggSpec *s, ExplainState *es)
{
	if (c->nfilter > 0)
		ExplainPropertyInteger("Filters In Source", NULL, c->nfilter, es);
	if (c->ntype > 0)
		ExplainPropertyInteger("Type Filters In Source", NULL, c->ntype, es);
	if (c->nwide > 0)
		ExplainPropertyInteger("Wide Filters In Source", NULL, c->nwide, es);
	if (s->njoin > 0)
	{
		ExplainPropertyInteger("Lookup Rows Read", NULL, c->lookup_read, es);
		ExplainPropertyInteger("Lookup Rows Loaded", NULL, c->lookup_loaded, es);
		if (c->lookup_shared > 0)
			ExplainPropertyInteger("Lookup Rows Shared", NULL, c->lookup_shared, es);
		if (c->lookup_index > 0)
			ExplainPropertyInteger("Lookup Index Scans", NULL, c->lookup_index, es);
		if (es->timing)
			ExplainPropertyFloat("Lookup Load Time", "ms", c->lookup_us / 1000.0, 3, es);
	}
	if (c->njfilter > 0)
		ExplainPropertyInteger("Join Key Filters", NULL, c->njfilter, es);
	if (c->njpass > 0)
		ExplainPropertyInteger("Join Key Passes", NULL, c->njpass, es);
	if (c->nshared > 0)
		ExplainPropertyInteger("Shared Lookup Tables", NULL, c->nshared, es);
	if (c->ncached > 0)
		ExplainPropertyInteger("Cached Lookup Tables", NULL, c->ncached, es);
	if (c->nfirst > 0)
		ExplainPropertyInteger("First Row Lookup Tables", NULL, c->nfirst, es);
	if (c->nsemi > 0)
	{
		ExplainPropertyInteger("Semi Lookup Tables", NULL, c->nsemi, es);
		ExplainPropertyInteger("Semi Lookup Table Size", "kB", (c->semi_bytes + 1023) / 1024, es);
	}
}

/* The units a run read and how many came from the pre-aggregates */
static void
explain_units(AggExec *ex, AggExec *c, ExplainState *es)
{
	sift_aggbe_explain(c->be, c->be_state, es, c->n_units);
	if (c->parts_pruned > 0)
		ExplainPropertyInteger("Partitions Removed", NULL, c->parts_pruned, es);
	ExplainPropertyInteger("Units", NULL, c->n_units, es);
	if (!c->pa_on && es->verbose)
		ExplainPropertyText("Pre-aggregates", sift_pawhy_name(c->pa_why), es);
	if (!c->pa_on)
		return;
	ExplainPropertyInteger("Units From Pre-aggregates", NULL, c->n_pre_units, es);
	if (ex->pa.list < 0)
		return;
	explain_pa_leaves(ex, es);
	ExplainPropertyInteger("Units Without The Rollup", NULL, c->n_rollup_miss, es);
	if (c->n_rollup_cost > 0)
		ExplainPropertyInteger("Units Scanned By Cost", NULL, c->n_rollup_cost, es);
	if (c->n_rollup_queued > 0)
		ExplainPropertyInteger("Rollup Refresh Queued", NULL, c->n_rollup_queued, es);
}

static void
explain_rows(AggExec *ex, AggExec *c, SiftSrcCounters *cc, ExplainState *es)
{
	ExplainPropertyInteger("Units Pruned", NULL, cc->units_pruned, es);
	ExplainPropertyInteger("Rows Scanned", NULL, c->n_rows, es);
	ExplainPropertyInteger("Tuples Aggregated", NULL, c->n_tuples, es);
	ExplainPropertyInteger("Dense Chunks", NULL, c->n_dense, es);
	ExplainPropertyInteger("Rows Vetted By Heap", NULL, cc->rows_vetted, es);
	ExplainPropertyInteger("Heap Pages", NULL, cc->heap_pages, es);
	if (c->recheck)
	{
		ExplainPropertyInteger("Units Read Again From The Heap", NULL, cc->recheck_units, es);
		ExplainPropertyInteger("Rows Read Again From The Heap", NULL, cc->rechecked, es);
	}
	ExplainPropertyInteger("Groups", NULL, ex->n_groups, es);
	if (c->n_osa_spilled)
		ExplainPropertyInteger("Ordered Sets Spilled", NULL, c->n_osa_spilled, es);
	if (c->n_dist_spilled)
		ExplainPropertyInteger("Distinct Sets Spilled", NULL, c->n_dist_spilled, es);
	if (c->n_dist_chunks)
		ExplainPropertyInteger("Distinct Chunks Merged", NULL, c->n_dist_chunks, es);
}

/* EXPLAIN ANALYZE: what the run did; the arms of a fused run report on its one pass */
static void
explain_run(AggExec *ex, ExplainState *es)
{
	SiftAggSpec *s = ex->spec;
	SiftSrcCounters cc;
	AggExec    *c = ex->fuse ? ex->fuse->F : ex;

	run_counters(c, &cc);
	if (ex->fuse)
		ExplainPropertyInteger("Arms In One Pass", NULL, ex->fuse->F->spec->nsets, es);
	ExplainPropertyText("SIMD", sift_simd_level_name(sift_cpu.active), es);
	if (s->nworkers > 0)
		ExplainPropertyInteger("Workers Launched", NULL, c->par ? c->par->nlaunched : 0, es);
	if (s->search)
		ExplainPropertyInteger("Search Matches", NULL, (c->search ? sift_search_matches(c->search) : 0) + (c->par ? c->par->matches : 0), es);
	if (s->topn && ex->topn)
	{
		ExplainPropertyInteger("Superblocks Skipped By Top-N", NULL, sift_topn_skipped(ex->topn), es);
		ExplainPropertyInteger("Heap Fetches", NULL, sift_topn_fetched(ex->topn), es);
		if (sift_topn_sorted(ex->topn) >= 0)
			ExplainPropertyInteger("Top-N Sorted Rows", NULL, sift_topn_sorted(ex->topn), es);
	}
	explain_source_run(c, s, es);
	explain_units(ex, c, es);
	explain_rows(ex, c, &cc, es);
}

static void
agg_explain(CustomScanState *node, List *ancestors, ExplainState *es)
{
	AggExec    *ex = (AggExec *) node;
	SiftAggSpec *s = ex->spec;

	explain_source(s, es);
	explain_keys(s, es);
	for (int i = 0; i < s->njoin; i++)
		if (s->join[i].subplan > 0)
			explain_correlated(node, i, es);
	if (s->nworkers > 0)
		ExplainPropertyInteger("Workers Planned", NULL, s->nworkers, es);
	if (es->analyze && ex->started)
		explain_run(ex, es);
}

static const CustomExecMethods agg_exec_methods = {
	.CustomName = "SiftAgg",
	.BeginCustomScan = agg_begin,
	.ExecCustomScan = agg_exec,
	.EndCustomScan = agg_end,
	.ReScanCustomScan = agg_rescan,
	.ExplainCustomScan = agg_explain,
};

Node *
sift_agg_create_state(CustomScan *cscan)
{
	AggExec    *ex = (AggExec *) newNode(sizeof(AggExec), T_CustomScanState);

	ex->css.methods = &agg_exec_methods;
	ex->css.flags = cscan->flags;
	return (Node *) ex;
}
