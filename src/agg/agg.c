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
#include "agg/spec.h"
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
int		sift_agg_semi_lut_size = 4096;
bool		sift_agg_lookup_pushdown = true;
bool		sift_agg_join_filter = true;
bool		sift_agg_share_lookups = true;
bool		sift_enable_topn = true;
static bool sift_test_agg_wait = false;
int		sift_agg_combo_cache = 1 << 20;

void
sift_agg_init(void)
{
	sift_aggkern_init();
	sift_aggcol_register();
	sift_aggcte_init();
	sift_search_init();
	DefineCustomBoolVariable("sift.enable_agg",
							 "Enables aggregate pushdown into sift_col column indexes (Custom Scan (SiftAgg)).",
							 NULL, &sift_enable_agg, true,
							 PGC_USERSET, 0, NULL, NULL, NULL);
	DefineCustomBoolVariable("sift.agg_preagg",
							 "Answers the units of an aggregate scan whose rows are all visible from the pre-aggregates the sift_col index stored with them.",
							 NULL, &sift_agg_preagg, true,
							 PGC_USERSET, 0, NULL, NULL, NULL);
	DefineCustomBoolVariable("sift.agg_dense",
							 "Aggregates the chunks of counts, sums and averages from the bitmap of their rows, without a list of tuples.",
							 NULL, &sift_agg_dense, true,
							 PGC_USERSET, GUC_NOT_IN_SAMPLE, NULL, NULL, NULL);
	DefineCustomBoolVariable("sift.agg_semi_lut",
							 "Answers a semi or anti join (IN, EXISTS, NOT EXISTS) on an integer leaf from a byte table over the keys of the joined table.",
							 NULL, &sift_agg_semi_lut, true,
							 PGC_USERSET, 0, NULL, NULL, NULL);
	DefineCustomBoolVariable("sift.agg_lookup_pushdown",
							 "Loads the rows a lookup join can match only: an index scan or scan keys on the table's own `column = value` conditions, and the other own conditions tested while loading.",
							 NULL, &sift_agg_lookup_pushdown, true,
							 PGC_USERSET, 0, NULL, NULL, NULL);
	DefineCustomBoolVariable("sift.agg_join_filter",
							 "Tests the integer key leaf of an inner or semi lookup join against the keys of the loaded table before tuples are made (zone pruning, source filters, row bitmaps).",
							 NULL, &sift_agg_join_filter, true,
							 PGC_USERSET, 0, NULL, NULL, NULL);
	DefineCustomBoolVariable("sift.agg_share_lookups",
							 "Lets the Custom Scan (SiftAgg) nodes of one statement that join the same table share the rows they load (UNION ALL arms).",
							 NULL, &sift_agg_share_lookups, true,
							 PGC_USERSET, 0, NULL, NULL, NULL);
	DefineCustomIntVariable("sift.agg_semi_lut_size",
							"Memory of the byte table of a semi or anti join, per scan and per worker; keys that span more keep the generic join.",
							NULL, &sift_agg_semi_lut_size, 4096, 0, 1024 * 1024,
							PGC_USERSET, GUC_UNIT_KB, NULL, NULL, NULL);
	DefineCustomBoolVariable("sift.enable_topn",
							 "Enables ORDER BY .. LIMIT from the column units (Custom Scan (SiftAgg) with Top-N), with sift.enable_agg.",
							 NULL, &sift_enable_topn, true,
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
		ex->ps[i].rowmask_ok = false;
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

static void
run_scan(AggExec *ex)
{
	bool		ordered = sift_aggspec_ordered(ex->spec);

	ex->null_input = false;
	ex->started = true;
	ex->glist = NULL;
	engine_open(ex, ex->snap, ex->params);
	scan_buffers(ex);
	if (!backend_begin(ex))
		par_launch(ex);
	if (ex->shared == NULL)
	{
		ex->shared = MemoryContextAlloc(ex->cxt, shared_size(ex->spec));
		shared_init(ex->shared, ex);
	}
	ex->lane_serial = ordered && ex->par == NULL;
	ex->lanes = ordered && !ex->lane_serial;
	table_init(ex);
	ex->gtab = ex->lanes ? result_view(ex) : ex;
	ex->pending = MemoryContextAllocZero(ex->cxt, SIFT_AGG_LANES * sizeof(char *));
	if (ex->par == NULL || parallel_leader_participation)
		scan_items(ex);
	backend_finish(ex);
	unmatched_tail(ex);
	arms_flush(ex);
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
static void
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

static Datum
key_datum(AggExec *ex, int j, uint32 g, bool *isnull)
{
	KeyState   *k = &ex->ks[j];
	uint32		id = key_id(ex, j, g);

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

static Datum
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

/*
 * ---------- arms that share one pass ----------
 *
 * The arms of a UNION ALL over one CTE (a time series at four granularities) are nodes that scan the same rows for the same aggregates and
 * group by different keys. The first of them to run finds the others (nodes of the same executor state register when they begin) and
 * scans for all of them as the grouping sets of one spec: the leaves are decoded and the filters run once, and every arm gets its groups
 * from the set that is its own. The sums are those of an arm alone (the same tuples in the same lanes), so the rows are the same.
 */

typedef struct FuseReg
{
	EState	   *estate;
	List	   *nodes;			/* the AggExec of the estate that have not started */
} FuseReg;

static List *fuse_regs;

static void
fuse_reg_free(void *arg)
{
	FuseReg    *r = arg;

	fuse_regs = list_delete_ptr(fuse_regs, r);
	list_free(r->nodes);
	pfree(r);
}

static FuseReg *
fuse_reg(EState *estate, bool create)
{
	FuseReg    *r;
	MemoryContextCallback *cb;
	MemoryContext old;

	foreach_ptr(FuseReg, x, fuse_regs)
		if (x->estate == estate)
			return x;
	if (!create)
		return NULL;
	old = MemoryContextSwitchTo(TopMemoryContext);
	r = palloc0(sizeof(FuseReg));
	r->estate = estate;
	fuse_regs = lappend(fuse_regs, r);
	MemoryContextSwitchTo(old);
	cb = MemoryContextAlloc(estate->es_query_cxt, sizeof(MemoryContextCallback));
	cb->func = fuse_reg_free;
	cb->arg = r;
	MemoryContextRegisterResetCallback(estate->es_query_cxt, cb);
	return r;
}

static void
fuse_register(AggExec *ex)
{
	FuseReg    *r = fuse_reg(ex->css.ss.ps.state, true);
	MemoryContext old = MemoryContextSwitchTo(TopMemoryContext);

	r->nodes = lappend(r->nodes, ex);
	MemoryContextSwitchTo(old);
}

static void
fuse_unregister(AggExec *ex)
{
	FuseReg    *r = fuse_reg(ex->css.ss.ps.state, false);

	if (r)
		r->nodes = list_delete_ptr(r->nodes, ex);
}

/* the nodes (this one first) that one pass can serve, scanned into the groups of a fused executor; false when this node runs alone */
static bool
fuse_start(AggExec *ex)
{
	FuseReg    *r = fuse_reg(ex->css.ss.ps.state, false);
	AggExec    *m[SIFT_AGG_MAX_FUSE];
	SiftAggSpec *specs[SIFT_AGG_MAX_FUSE];
	int			keyoff[SIFT_AGG_MAX_FUSE],
				n = 0,
				nk = 1;
	EState	   *estate = ex->css.ss.ps.state;
	MemoryContext old;
	FuseSt	   *fs;
	AggExec    *F;

	if (r == NULL)
		return false;
	m[n] = ex;
	specs[n++] = ex->spec;
	nk += ex->spec->nkeys;
	foreach_ptr(AggExec, o, r->nodes)
		if (o != ex && !o->started && nk + o->spec->nkeys <= SIFT_AGG_MAX_FUSE && sift_aggspec_fusable(ex->spec, o->spec))
		{
			m[n] = o;
			specs[n++] = o->spec;
			nk += o->spec->nkeys;
		}
	if (n < 2)
		return false;
	old = MemoryContextSwitchTo(estate->es_query_cxt);
	F = palloc0(sizeof(AggExec));
	F->spec = sift_aggspec_fuse(specs, n, keyoff);
	F->pack = sift_aggspec_pack(F->spec);
	F->params = ex->params;
	F->snap = ex->snap;
	fs = palloc0(sizeof(FuseSt));
	fs->F = F;
	fs->refs = n;
	engine_contexts(F, estate->es_query_cxt);
	engine_rels(F);
	MemoryContextSwitchTo(old);
	run_scan(F);
	empty_set_groups(F->gtab);
	dist_finish_all(F->gtab);
	dist_end_all(F->gtab);
	mapped_finish(F->gtab);
	list_groups(F->gtab);
	for (int i = 0; i < n; i++)
	{
		m[i]->fuse = fs;
		m[i]->fuse_set = i;
		m[i]->fuse_off = keyoff[i];
		m[i]->started = true;
		r->nodes = list_delete_ptr(r->nodes, m[i]);
	}
	return true;
}

/* the groups of the fused scan that are this node's set */
static void
fuse_list(AggExec *ex)
{
	AggExec    *t = ex->fuse->F->gtab;
	int			setkey = t->spec->nkeys - 1;

	ex->glist = MemoryContextAlloc(ex->cxt, Max(t->nglist, 1) * sizeof(uint32));
	ex->nglist = 0;
	for (uint32 i = 0; i < t->nglist; i++)
		if (key_id(t, setkey, t->glist[i]) == (uint32) ex->fuse_set + 1)
			ex->glist[ex->nglist++] = t->glist[i];
	ex->n_groups = ex->nglist;
}

static TupleTableSlot *
fuse_next(AggExec *ex, TupleTableSlot *slot)
{
	AggExec    *t = ex->fuse->F->gtab;
	uint32		g;

	if (ex->glist == NULL)
		fuse_list(ex);
	if (ex->gpos >= ex->nglist)
		return NULL;
	g = ex->glist[ex->gpos++];
	ExecClearTuple(slot);
	for (int j = 0; j < ex->spec->nkeys; j++)
		slot->tts_values[j] = key_datum(t, ex->fuse_off + j, g, &slot->tts_isnull[j]);
	for (int i = 0; i < ex->spec->naggs; i++)
	{
		int			c = ex->spec->nkeys + i;

		slot->tts_values[c] = agg_datum(t, i, g, &slot->tts_isnull[c]);
	}
	return ExecStoreVirtualTuple(slot);
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

/* ---------- executor methods ---------- */

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
		engine_close(ex->fuse->F);
	dist_end_all(ex);
	if (ex->gtab)
		dist_end_all(ex->gtab);
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

static void
agg_explain(CustomScanState *node, List *ancestors, ExplainState *es)
{
	AggExec    *ex = (AggExec *) node;
	SiftAggSpec *s = ex->spec;
	StringInfoData buf;
	int			h1 = s->nkeys - (s->nsets > 0),	/* the hidden keys are between the outer ones and the set's */
				h0 = h1 - s->nhidden;

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
	for (int i = 0; i < s->njoin; i++)
		if (s->join[i].subplan > 0)
		{
			EState     *estate = node->ss.ps.state;
			QueryDesc   query = {0};
			PlannedStmt stmt = *estate->es_plannedstmt;
			ExplainState child;
			char       *label = psprintf("Correlated Lookup %d", i + 1);

			ExplainOpenGroup("Correlated Lookup", label, true, es);
			ExplainPropertyText("Name", s->join[i].desc, es);
			/* the statement's identifier and settings are printed once, at the top */
			stmt.queryId = UINT64CONST(0);
			query.plannedstmt = &stmt;
			query.planstate = list_nth(estate->es_subplanstates, s->join[i].subplan - 1);
			query.estate = estate;
			child = *es;
			child.settings = false;
			child.printed_subplans = NULL;
			ExplainPrintPlan(&child, &query);
			es->indent = child.indent;
			es->grouping_stack = child.grouping_stack;
			ExplainCloseGroup("Correlated Lookup", label, true, es);
		}
	if (s->nworkers > 0)
		ExplainPropertyInteger("Workers Planned", NULL, s->nworkers, es);
	if (es->analyze && ex->started)
	{
		SiftSrcCounters cc;
		AggExec    *c = ex->fuse ? ex->fuse->F : ex;

		sift_colsrc_counters(c->src, &cc);
		if (ex->fuse)
			ExplainPropertyInteger("Arms In One Pass", NULL, ex->fuse->F->spec->nsets, es);
		ExplainPropertyText("SIMD", sift_simd_level_name(sift_cpu.active), es);
		if (s->nworkers > 0)
			ExplainPropertyInteger("Workers Launched", NULL, c->par ? c->par->nlaunched : 0, es);
		if (c->par)
		{
			cc.units_pruned += c->par->cc.units_pruned;
			cc.rows_vetted += c->par->cc.rows_vetted;
			cc.heap_pages += c->par->cc.heap_pages;
			cc.rechecked += c->par->cc.rechecked;
		}
		if (s->search)
			ExplainPropertyInteger("Search Matches", NULL, (c->search ? sift_search_matches(c->search) : 0) + (c->par ? c->par->matches : 0), es);
		if (s->topn && ex->topn)
		{
			ExplainPropertyInteger("Superblocks Skipped By Top-N", NULL, sift_topn_skipped(ex->topn), es);
			ExplainPropertyInteger("Heap Fetches", NULL, sift_topn_fetched(ex->topn), es);
			if (sift_topn_sorted(ex->topn) >= 0)
				ExplainPropertyInteger("Top-N Sorted Rows", NULL, sift_topn_sorted(ex->topn), es);
		}
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
			if (c->lookup_index > 0)
				ExplainPropertyInteger("Lookup Index Scans", NULL, c->lookup_index, es);
			if (es->timing)
				ExplainPropertyFloat("Lookup Load Time", "ms", c->lookup_us / 1000.0, 3, es);
		}
		if (c->njfilter > 0)
			ExplainPropertyInteger("Join Key Filters", NULL, c->njfilter, es);
		if (c->nshared > 0)
			ExplainPropertyInteger("Shared Lookup Tables", NULL, c->nshared, es);
		if (c->nsemi > 0)
		{
			ExplainPropertyInteger("Semi Lookup Tables", NULL, c->nsemi, es);
			ExplainPropertyInteger("Semi Lookup Table Size", "kB", (c->semi_bytes + 1023) / 1024, es);
		}
		sift_aggbe_explain(c->be, c->be_state, es, c->n_units);
		ExplainPropertyInteger("Units", NULL, c->n_units, es);
		if (c->pa_on)
		{
			ExplainPropertyInteger("Units From Pre-aggregates", NULL, c->n_pre_units, es);
			if (ex->pa.list >= 0)
			{
				explain_pa_leaves(ex, es);
				ExplainPropertyInteger("Units Without The Rollup", NULL, c->n_rollup_miss, es);
				if (c->n_rollup_queued > 0)
					ExplainPropertyInteger("Rollup Refresh Queued", NULL, c->n_rollup_queued, es);
			}
		}
		ExplainPropertyInteger("Units Pruned", NULL, cc.units_pruned, es);
		ExplainPropertyInteger("Rows Scanned", NULL, c->n_rows, es);
		ExplainPropertyInteger("Tuples Aggregated", NULL, c->n_tuples, es);
		ExplainPropertyInteger("Dense Chunks", NULL, c->n_dense, es);
		ExplainPropertyInteger("Rows Vetted By Heap", NULL, cc.rows_vetted, es);
		ExplainPropertyInteger("Heap Pages", NULL, cc.heap_pages, es);
		if (ex->recheck)
			ExplainPropertyInteger("Rows Read Again From The Heap", NULL, cc.rechecked, es);
		ExplainPropertyInteger("Groups", NULL, ex->n_groups, es);
		if (c->n_osa_spilled)
			ExplainPropertyInteger("Ordered Sets Spilled", NULL, c->n_osa_spilled, es);
		if (c->n_dist_spilled)
			ExplainPropertyInteger("Distinct Sets Spilled", NULL, c->n_dist_spilled, es);
		if (c->n_dist_chunks)
			ExplainPropertyInteger("Distinct Chunks Merged", NULL, c->n_dist_chunks, es);
	}
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
