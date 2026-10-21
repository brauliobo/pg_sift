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
static bool sift_agg_semi_lut = true;
static int	sift_agg_semi_lut_size = 4096;
bool		sift_agg_lookup_pushdown = true;
static bool sift_agg_join_filter = true;
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

static bool
param_walker(Node *n, Param **found)
{
	if (n == NULL)
		return false;
	if (IsA(n, Param) && ((Param *) n)->paramkind == PARAM_EXEC && ((Param *) n)->paramid < SIFT_AGG_INIT_PARAM)
	{
		*found = (Param *) n;
		return true;
	}
	return expression_tree_walker(n, param_walker, found);
}

/* the placeholder Param 0 of an expression: the type its input has */
static Param *
find_param(Node *expr)
{
	Param	   *p = NULL;

	param_walker(expr, &p);
	if (p == NULL)
		elog(ERROR, "sift: aggregate plan expression without its input");
	return p;
}

/*
 * The output of an ARRAY sub-select (an InitPlan of the query, its Param offset by SIFT_AGG_INIT_PARAM) is the constant of the expression: the sub-select
 * runs now, the first time the lookup table needs it as in the normal plan, and the lookup table's own parameters are not the executor's.
 */
static Node *
init_param_mutator(Node *n, AggExec *ex)
{
	Param	   *p = (Param *) n;
	EState	   *estate = ex->css.ss.ps.state;
	ParamExecData *prm;
	int16		len;
	bool		byval;

	if (n == NULL)
		return NULL;
	if (!IsA(n, Param) || p->paramkind != PARAM_EXEC || p->paramid < SIFT_AGG_INIT_PARAM)
		return expression_tree_mutator(n, init_param_mutator, ex);
	prm = &estate->es_param_exec_vals[p->paramid - SIFT_AGG_INIT_PARAM];
	if (prm->execPlan != NULL)
		ExecSetParamPlan(prm->execPlan, GetPerTupleExprContext(estate));
	get_typlenbyval(p->paramtype, &len, &byval);
	return (Node *) makeConst(p->paramtype, p->paramtypmod, p->paramcollid, len, prm->value, prm->isnull, byval);
}

/*
 * The expression of a lookup table. A sub-plan in it (its state, hash table and the scans it starts) lives in the query's memory: PostgreSQL ends
 * the sub-plans after this node, which frees its own memory.
 */
static ExprState *
init_lut_expr(AggExec *ex, Node *expr)
{
	MemoryContext old = MemoryContextSwitchTo(contain_subplans(expr) ? ex->css.ss.ps.state->es_query_cxt : ex->cxt);
	ExprState  *e = ExecInitExpr((Expr *) init_param_mutator(expr, ex), EXPR_PARENT(ex));

	MemoryContextSwitchTo(old);
	return e;
}

static void
extend_layout(AggExec *ex)
{
	SiftAggSpec *s = ex->spec;
	SiftSrcLayout *lay = MemoryContextAlloc(ex->cxt, sizeof(SiftSrcLayout));

	ex->nbase = ex->lay->nleaves;
	if (s->nvleaf == 0)
		return;
	*lay = *ex->lay;
	lay->leaf = MemoryContextAllocZero(ex->cxt, (ex->nbase + s->nvleaf) * sizeof(SiftSrcLeaf));
	memcpy(lay->leaf, ex->lay->leaf, ex->nbase * sizeof(SiftSrcLeaf));
	for (int j = 0; j < s->nvleaf; j++)
	{
		SiftSrcLeaf *v = &lay->leaf[ex->nbase + j];

		for (int i = 0; i < ex->nbase && sift_aggvleaf_member(&s->vleaf[j]); i++)
			if (lay->leaf[i].parent == s->vleaf[j].obj)
				*(strcmp(lay->leaf[i].name, "key") == 0 ? &ex->vl[j].key : &ex->vl[j].val) = i;
		v->id = ex->nbase + j;
		v->attno = lay->leaf[s->vleaf[j].obj].attno;
		v->parent = -1;
		v->kind = SCL_SCALAR;
		v->vtype = s->vleaf[j].types ? SCV_CODE : s->vleaf[j].count ? SCV_I32 : lay->leaf[ex->vl[j].val].vtype;
		v->typid = s->vleaf[j].types ? TEXTOID : s->vleaf[j].count ? INT4OID : lay->leaf[ex->vl[j].val].typid;
	}
	lay->nleaves += s->nvleaf;
	ex->lay = lay;
}

static void
overflow_error(void)
{
	ereport(ERROR,
			(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
			 errmsg("sift: a column of the aggregate scan holds values its index cannot store"),
			 errhint("Set sift.enable_agg to off for this query.")));
}

/* bits [lo, hi) of a bit vector cleared */
static void
clear_bit_range(uint64 *bits, uint32 lo, uint32 hi)
{
	for (; lo < hi && (lo & 63); lo++)
		bits[lo >> 6] &= ~(UINT64CONST(1) << (lo & 63));
	for (; lo + 64 <= hi; lo += 64)
		bits[lo >> 6] = 0;
	for (; lo < hi; lo++)
		bits[lo >> 6] &= ~(UINT64CONST(1) << (lo & 63));
}

/* the lookup of the aggregate's functions, once per scan */
static void
pgagg_init(AggExec *ex)
{
	SiftAggSpec *s = ex->spec;

	ex->pgagg = palloc0(Max(s->naggs, 1) * sizeof(PgAgg *));
	for (int ai = 0; ai < s->naggs; ai++)
	{
		const SiftAggAgg *a = &s->agg[ai];
		HeapTuple	tup;
		Form_pg_aggregate ag;
		PgAgg	   *pa;
		bool		isnull;
		Datum		init;

		if (a->kind != SAA_M_PG)
			continue;
		if (ex->pgstate == NULL)
		{
			ex->pgstate = makeNode(AggState);
			ex->pgstate->curaggcontext = CreateStandaloneExprContext();
		}
		tup = SearchSysCache1(AGGFNOID, ObjectIdGetDatum(a->aggfn));
		if (!HeapTupleIsValid(tup))
			elog(ERROR, "cache lookup failed for aggregate %u", a->aggfn);
		ag = (Form_pg_aggregate) GETSTRUCT(tup);
		pa = ex->pgagg[ai] = palloc0(sizeof(PgAgg));
		fmgr_info(ag->aggtransfn, &pa->trans);
		pa->strict = pa->trans.fn_strict;
		pa->has_final = OidIsValid(ag->aggfinalfn);
		if (pa->has_final)
			fmgr_info(ag->aggfinalfn, &pa->final);
		pa->final_extra = ag->aggfinalextra;
		get_typlenbyval(ag->aggtranstype, &pa->tlen, &pa->tbyval);
		get_typlenbyval(a->restype, &pa->rlen, &pa->rbyval);
		init = SysCacheGetAttr(AGGFNOID, tup, Anum_pg_aggregate_agginitval, &isnull);
		pa->init_null = isnull;
		if (!isnull)
		{
			Oid			typinput,
						typioparam;

			getTypeInputInfo(ag->aggtranstype, &typinput, &typioparam);
			pa->init = OidInputFunctionCall(typinput, TextDatumGetCString(init), typioparam, -1);
		}
		ReleaseSysCache(tup);
	}
}

/* the state of group g gets `times` more occurrences of val: the transition function as nodeAgg calls it (strictness, initial value, the state copied out of the call's memory) */
static void
pgagg_add(AggExec *ex, int ai, uint32 g, Datum val, int64 times)
{
	PgAgg	   *pa = ex->pgagg[ai];
	AggAcc	   *acc = &ex->acc[ai];
	MemoryContext aggcxt = ex->pgstate->curaggcontext->ecxt_per_tuple_memory;
	LOCAL_FCINFO(fc, 2);

	for (int64 t = 0; t < times; t++)
	{
		Datum		r;
		MemoryContext old;

		if (!acc->nf[g] && !pa->init_null)
		{
			old = MemoryContextSwitchTo(aggcxt);
			acc->v[g] = datumCopy(pa->init, pa->tbyval, pa->tlen);
			MemoryContextSwitchTo(old);
			acc->nf[g] = 1;
		}
		else if (!acc->nf[g] && pa->strict)
		{
			old = MemoryContextSwitchTo(aggcxt);
			acc->v[g] = datumCopy(val, pa->tbyval, pa->tlen);
			MemoryContextSwitchTo(old);
			acc->nf[g] = 1;
			continue;
		}
		InitFunctionCallInfoData(*fc, &pa->trans, 2, ex->spec->agg[ai].collid, (Node *) ex->pgstate, NULL);
		fc->args[0].value = acc->v[g];
		fc->args[0].isnull = !acc->nf[g];
		fc->args[1].value = val;
		fc->args[1].isnull = false;
		old = MemoryContextSwitchTo(ex->comcxt);
		r = FunctionCallInvoke(fc);
		MemoryContextSwitchTo(aggcxt);
		if (fc->isnull)
		{
			acc->nf[g] = 0;
			acc->v[g] = (Datum) 0;
		}
		else if (pa->tbyval)
		{
			acc->v[g] = r;
			acc->nf[g] = 1;
		}
		else if (DatumGetPointer(r) != DatumGetPointer(acc->v[g]))
		{
			Datum		prev = acc->v[g];
			bool		had = acc->nf[g];

			acc->v[g] = datumCopy(r, false, pa->tlen);
			acc->nf[g] = 1;
			if (had)
				pfree(DatumGetPointer(prev));
		}
		else
			acc->nf[g] = 1;
		MemoryContextSwitchTo(old);
		MemoryContextReset(ex->comcxt);
	}
}

/* the result of group g: the final function on its state (the state itself without one), kept in acc->v; acc->nf says whether it is not NULL */
static void
pgagg_final(AggExec *ex, int ai, uint32 g)
{
	PgAgg	   *pa = ex->pgagg[ai];
	AggAcc	   *acc = &ex->acc[ai];
	LOCAL_FCINFO(fc, 2);
	Datum		r;
	bool		isnull;
	MemoryContext old;

	if (!pa->has_final)
		return;
	if (pa->final.fn_strict && !acc->nf[g])
	{
		acc->v[g] = (Datum) 0;
		return;
	}
	InitFunctionCallInfoData(*fc, &pa->final, pa->final_extra ? 2 : 1, ex->spec->agg[ai].collid, (Node *) ex->pgstate, NULL);
	fc->args[0].value = acc->v[g];
	fc->args[0].isnull = !acc->nf[g];
	fc->args[1].value = (Datum) 0;
	fc->args[1].isnull = true;
	old = MemoryContextSwitchTo(ex->comcxt);
	r = FunctionCallInvoke(fc);
	isnull = fc->isnull;
	MemoryContextSwitchTo(ex->tcxt);
	acc->v[g] = isnull ? (Datum) 0 : datumCopy(r, pa->rbyval, pa->rlen);
	acc->nf[g] = !isnull;
	MemoryContextSwitchTo(old);
	MemoryContextReset(ex->comcxt);
}

/* `times` rows of group g had the value id: min, max, sum or avg of the aggregate ai */
static void
mapped_add(AggExec *ex, int ai, uint32 g, uint32 id, int64 times)
{
	SiftAggAgg *a = &ex->spec->agg[ai];
	AggAcc	   *acc = &ex->acc[ai];
	Datum		val = ex->aot[ai].val[id - 1];

	switch (a->kind)
	{
		case SAA_M_PG:
			pgagg_add(ex, ai, g, val, times);
			acc->n[g] += times;
			break;
		case SAA_M_MIN:
		case SAA_M_MAX:
			{
				FmgrInfo   *cmp = &lookup_type_cache(a->argtype, TYPECACHE_CMP_PROC_FINFO)->cmp_proc_finfo;
				int32		c = acc->n[g] ? DatumGetInt32(FunctionCall2Coll(cmp, a->collid, val, acc->v[g])) : 0;

				if (acc->n[g] == 0 || (a->kind == SAA_M_MIN ? c < 0 : c > 0))
					acc->v[g] = val;
				acc->n[g] = 1;
			}
			break;
		default:
			if (a->restype == NUMERICOID)
			{
				Datum		sum = DirectFunctionCall2(numeric_mul, DirectFunctionCall1(int8_numeric, Int64GetDatum(times)), val);

				acc->v[g] = acc->n[g] ? DirectFunctionCall2(numeric_add, acc->v[g], sum) : sum;
			}
			else
			{
				double		x = DatumGetFloat8(val);

				acc->d[g] += times * x;
				if (!isfinite(x))
					acc->nf[g] = 1;
				else
					ex->amax[ai] = Max(ex->amax[ai], fabs(x));
			}
			acc->n[g] += times;
	}
}

/* the results of the aggregates over distinct values, from the pairs of their sets */
static void
mapped_finish(AggExec *ex)
{
	MemoryContext old = MemoryContextSwitchTo(ex->tcxt);

	for (int ai = 0; ai < ex->spec->naggs; ai++)
	{
		const SiftAggAgg *a = &ex->spec->agg[ai];
		const DSet *d = &ex->dset[ai];

		if ((a->kind >= SAA_M_MIN && a->kind <= SAA_M_AVG) || a->kind == SAA_M_PG)
			for (uint64 i = 0; i < d->cap; i++)
				if (d->slot[i])
					mapped_add(ex, ai, (d->slot[i] >> 32) & 0x7fffffff, (uint32) d->slot[i], d->counted ? d->cnt[i] : 1);
		for (uint32 g = 0; a->kind == SAA_M_PG && g < (ex->hash ? ex->ngroups : ex->gcap); g++)
			if (ex->acc[ai].n[g] > 0)
				pgagg_final(ex, ai, g);
		if ((a->kind == SAA_M_AVG || a->kind == SAA_X_AVG) && a->restype == FLOAT8OID &&
			8.0 * ex->n_tuples * ex->amax[ai] * ex->amax[ai] >= DBL_MAX)
			float_overflow_error();
	}
	MemoryContextSwitchTo(old);
}

static Datum
mapped_sum_datum(AggExec *ex, const SiftAggAgg *a, int ai, uint32 g, bool *isnull)
{
	AggAcc	   *acc = &ex->acc[ai];
	bool		sum = a->kind == SAA_M_SUM || a->kind == SAA_X_SUM;

	*isnull = acc->n[g] == 0;
	if (*isnull)
		return (Datum) 0;
	if (a->restype == NUMERICOID)
	{
		Datum		tot = acc->nf[g] || a->kind == SAA_M_SUM || a->kind == SAA_M_AVG ? acc->v[g] : scaled_numeric(acc->w[g], (int) acc->i[g]);

		return sum ? tot : DirectFunctionCall2(numeric_div, tot, DirectFunctionCall1(int8_numeric, Int64GetDatum(acc->n[g])));
	}
	if (isinf(acc->d[g]) && !acc->nf[g])
		float_overflow_error();
	return Float8GetDatum(sum ? acc->d[g] : acc->d[g] / (double) acc->n[g]);
}

/* GROUPING(args) in a group of grouping set s: bit i (the first argument is the highest) is 1 when argument i is not grouped by the set */
static int32
grouping_bits(AggExec *ex, const SiftAggAgg *a, uint32 g)
{
	uint32		mask = ex->spec->setmask[key_id(ex, ex->spec->nkeys - 1, g) - 1];
	int32		bits = 0;

	for (int i = 0; i < a->nref; i++)
		bits = bits << 1 | !(mask >> a->ref[i] & 1);
	return bits;
}

/* ---------- generic aggregates (SAA_GEN): PostgreSQL's own transition and final functions ---------- */

static Datum
gen_eval_once(AggExec *ex, ExprState *e, Oid typid, bool *isnull)
{
	int16		len;
	bool		byval;
	Datum		d = ExecEvalExprSwitchContext(e, ex->lutcx, isnull);

	get_typlenbyval(typid, &len, &byval);
	d = *isnull ? (Datum) 0 : datumCopy(d, byval, len);
	ResetExprContext(ex->lutcx);
	return d;
}

/*
 * ORDER BY or DISTINCT in the call: a tuplesort of (group number, the arguments that vary) on the group number and the sort keys,
 * as nodeAgg sorts the input of such an aggregate (sort operator, input collation and NULLS FIRST/LAST of each key; DISTINCT sorts on
 * its own list, which starts with the ORDER BY keys); work_mem bounded. DISTINCT then drops an input equal to the one before it.
 */
static void
gen_sort_begin(GenSt *st)
{
	const SiftAggGen *g = st->g;
	Aggref	   *ref = st->aggref;
	List	   *sl = g->ordered ? NIL : ref->aggdistinct != NIL ? ref->aggdistinct : ref->aggorder;	/* WITHIN GROUP: the transition function sorts */
	int			nv = 0,
				nk = 1;
	AttrNumber	att[SIFT_AGG_MAX_GARGS + 1] = {1};
	Oid			ops[SIFT_AGG_MAX_GARGS + 1] = {Int4LessOperator},
				coll[SIFT_AGG_MAX_GARGS + 1] = {InvalidOid};
	bool		nf[SIFT_AGG_MAX_GARGS + 1] = {false};
	TupleDesc	desc;

	for (int k = 0; k < g->nargs; k++)
		st->vcol[k] = g->arg[k].nleaves > 0 || g->arg[k].mapped ? nv++ : -1;
	desc = CreateTemplateTupleDesc(1 + nv);
	TupleDescInitEntry(desc, 1, "gid", INT4OID, -1, 0);
	for (int k = 0; k < g->nargs; k++)
		if (st->vcol[k] >= 0)
			TupleDescInitEntry(desc, 2 + st->vcol[k], "a", g->arg[k].typid, -1, 0);
	foreach_node(SortGroupClause, sc, sl)
	{
		int			k = 0;
		TargetEntry *te = get_sortgroupref_tle(sc->tleSortGroupRef, ref->args);

		while (list_nth(ref->args, k) != te)
			k++;
		if (st->vcol[k] < 0)
			continue;
		att[nk] = 2 + st->vcol[k];
		ops[nk] = sc->sortop;
		coll[nk] = exprCollation((Node *) te->expr);
		nf[nk] = sc->nulls_first;
		nk++;
		if (g->distinct)
		{
			fmgr_info(get_opcode(sc->eqop), &st->deq[st->ndcol]);
			get_typlenbyval(g->arg[k].typid, &st->dlen[st->ndcol], &st->dbyval[st->ndcol]);
			st->dcol[st->ndcol++] = st->vcol[k];
		}
	}
	sift_tupdesc_finalize(desc);
	st->pcx = AllocSetContextCreate(CurrentMemoryContext, "sift agg distinct", ALLOCSET_SMALL_SIZES);
	st->put = MakeSingleTupleTableSlot(desc, &TTSOpsVirtual);
	st->get = MakeSingleTupleTableSlot(desc, &TTSOpsMinimalTuple);
	st->sort = tuplesort_begin_heap(desc, nk, att, ops, coll, nf, work_mem, NULL, TUPLESORT_NONE);
	st->tmode = true;
}

/* an input of the sort path: its group's number (given when the group's first input arrives) and the arguments that vary */
static void
gen_sort_put(GenSt *st, AggAcc *acc, uint32 gid, const Datum *args, const bool *nulls)
{
	if (acc->i[gid] == 0)
		acc->i[gid] = ++st->nsid;
	ExecClearTuple(st->put);
	st->put->tts_values[0] = Int32GetDatum((int32) (acc->i[gid] - 1));
	st->put->tts_isnull[0] = false;
	for (int k = 0; k < st->g->nargs; k++)
		if (st->vcol[k] >= 0)
		{
			st->put->tts_values[1 + st->vcol[k]] = args[k];
			st->put->tts_isnull[1 + st->vcol[k]] = nulls[k];
		}
	ExecStoreVirtualTuple(st->put);
	tuplesort_puttupleslot(st->sort, st->put);
}

/* DISTINCT ON: the sort support of each ORDER BY key of the call (operator, input collation and NULLS FIRST/LAST, as the sort of nodeAgg has them) */
static void
gen_first_begin(GenSt *st)
{
	const SiftAggGen *g = st->g;
	Aggref	   *ref = st->aggref;

	st->fsup = palloc0(Max(list_length(ref->aggorder), 1) * sizeof(SortSupportData));
	foreach_node(SortGroupClause, sc, ref->aggorder)
	{
		TargetEntry *te = get_sortgroupref_tle(sc->tleSortGroupRef, ref->args);
		SortSupport ssup = &st->fsup[st->nfk];
		int			k = 0;

		while (list_nth(ref->args, k) != te)
			k++;
		ssup->ssup_cxt = CurrentMemoryContext;
		ssup->ssup_collation = exprCollation((Node *) te->expr);
		ssup->ssup_nulls_first = sc->nulls_first;
		PrepareSortSupportFromOrderingOp(sc->sortop, ssup);
		st->fkey[st->nfk++] = k;
		st->iskey[k] = true;
	}
	for (int k = 0; k < g->nargs; k++)
		get_typlenbyval(g->arg[k].typid, &st->alen[k], &st->abyval[k]);
}

static GenSt *
gen_new(AggExec *ex, const SiftAggAgg *a, int ai)
{
	GenSt	   *st = palloc0(sizeof(GenSt));
	const SiftAggGen *g = a->gen;
	Aggref	   *ref = (Aggref *) g->aggref;
	HeapTuple	tup,
				proc;
	Form_pg_aggregate ag;
	Oid			owner,
				inputTypes[FUNC_MAX_ARGS];
	int			numArguments = get_aggregate_argtypes(ref, inputTypes),
				ntrans = 1 + g->ntrans;
	Expr	   *transfnexpr,
			   *finalfnexpr;
	AclResult	ar;
	int			k = 0;

	st->g = g;
	st->ai = ai;
	st->aggref = ref;
	ar = object_aclcheck(ProcedureRelationId, ref->aggfnoid, GetUserId(), ACL_EXECUTE);
	if (ar != ACLCHECK_OK)
		aclcheck_error(ar, OBJECT_AGGREGATE, get_func_name(ref->aggfnoid));
	tup = SearchSysCache1(AGGFNOID, ObjectIdGetDatum(ref->aggfnoid));
	proc = SearchSysCache1(PROCOID, ObjectIdGetDatum(ref->aggfnoid));
	if (!HeapTupleIsValid(tup) || !HeapTupleIsValid(proc))
		elog(ERROR, "cache lookup failed for aggregate %u", ref->aggfnoid);
	ag = (Form_pg_aggregate) GETSTRUCT(tup);
	owner = ((Form_pg_proc) GETSTRUCT(proc))->proowner;
	ReleaseSysCache(proc);
	foreach_oid(fn, list_make2_oid(ag->aggtransfn, ag->aggfinalfn))
	{
		if (!OidIsValid(fn))
			continue;
		ar = object_aclcheck(ProcedureRelationId, fn, owner, ACL_EXECUTE);
		if (ar != ACLCHECK_OK)
			aclcheck_error(ar, OBJECT_FUNCTION, get_func_name(fn));
	}
	st->transtype = resolve_aggregate_transtype(ref->aggfnoid, ag->aggtranstype, inputTypes, numArguments);
	get_typlenbyval(st->transtype, &st->tlen, &st->tbyval);
	get_typlenbyval(ref->aggtype, &st->rlen, &st->rbyval);
	build_aggregate_transfn_expr(inputTypes, numArguments, list_length(ref->aggdirectargs), ref->aggvariadic, st->transtype,
								 ref->inputcollid, ag->aggtransfn, InvalidOid, &transfnexpr, NULL);
	fmgr_info(ag->aggtransfn, &st->transfn);
	fmgr_info_set_expr((Node *) transfnexpr, &st->transfn);
	st->strict = st->transfn.fn_strict;
	st->tfc = palloc0(SizeForFunctionCallInfo(ntrans));
	InitFunctionCallInfoData(*st->tfc, &st->transfn, ntrans, ref->inputcollid, (Node *) ex->gas, NULL);
	st->ndirect = list_length(ref->aggdirectargs);
	if (OidIsValid(ag->aggfinalfn))
	{
		st->has_final = true;
		st->nfinal = ag->aggfinalextra ? numArguments + 1 : st->ndirect + 1;
		build_aggregate_finalfn_expr(inputTypes, st->nfinal, st->transtype, ref->aggtype, ref->inputcollid, ag->aggfinalfn,
									 &finalfnexpr);
		fmgr_info(ag->aggfinalfn, &st->finalfn);
		fmgr_info_set_expr((Node *) finalfnexpr, &st->finalfn);
		st->ffc = palloc0(SizeForFunctionCallInfo(st->nfinal));
		InitFunctionCallInfoData(*st->ffc, &st->finalfn, st->nfinal, ref->inputcollid, (Node *) ex->gas, NULL);
	}
	if (g->parallel)
	{
		Expr	   *e;

		fmgr_info(ag->aggcombinefn, &st->combinefn);
		st->cfc = palloc0(SizeForFunctionCallInfo(2));
		InitFunctionCallInfoData(*st->cfc, &st->combinefn, 2, ref->inputcollid, (Node *) ex->gas, NULL);
		if (OidIsValid(ag->aggserialfn))
		{
			build_aggregate_serialfn_expr(ag->aggserialfn, &e);
			fmgr_info(ag->aggserialfn, &st->serialfn);
			fmgr_info_set_expr((Node *) e, &st->serialfn);
			st->sfc = palloc0(SizeForFunctionCallInfo(1));
			InitFunctionCallInfoData(*st->sfc, &st->serialfn, 1, InvalidOid, (Node *) ex->gas, NULL);
			build_aggregate_deserialfn_expr(ag->aggdeserialfn, &e);
			fmgr_info(ag->aggdeserialfn, &st->deserialfn);
			fmgr_info_set_expr((Node *) e, &st->deserialfn);
			st->dfc = palloc0(SizeForFunctionCallInfo(2));
			InitFunctionCallInfoData(*st->dfc, &st->deserialfn, 2, InvalidOid, (Node *) ex->gas, NULL);
		}
	}
	{
		Datum		txt = SysCacheGetAttr(AGGFNOID, tup, Anum_pg_aggregate_agginitval, &st->initnull);

		if (!st->initnull)
		{
			Oid			infn,
						ioparam;

			getTypeInputInfo(st->transtype, &infn, &ioparam);
			st->initval = OidInputFunctionCall(infn, TextDatumGetCString(txt), ioparam, -1);
		}
	}
	ReleaseSysCache(tup);
	foreach_ptr(Node, d, ref->aggdirectargs)
	{
		st->dir[k] = gen_eval_once(ex, ExecInitExpr((Expr *) d, EXPR_PARENT(ex)), exprType(d), &st->dirnull[k]);
		k++;
	}
	for (k = 0; k < g->nargs; k++)
	{
		const SiftAggGenArg *ar2 = &g->arg[k];

		if (ar2->nleaves == 0 && !ar2->mapped)
			st->argk[k] = gen_eval_once(ex, ExecInitExpr((Expr *) ar2->expr, EXPR_PARENT(ex)), ar2->typid, &st->argknull[k]);
		else if (ar2->expr)
			st->argx[k] = ExecInitExpr((Expr *) ar2->expr, EXPR_PARENT(ex));
	}
	st->cx = CreateStandaloneExprContext();
	st->pertrans.aggref = ref;
	st->pertrans.aggshared = false;
	st->peragg.aggref = ref;
	st->peragg.transno = 0;
	if (g->distinct)
	{
		TypeCacheEntry *tc = lookup_type_cache(g->arg[g->darg].typid, TYPECACHE_CMP_PROC_FINFO | TYPECACHE_EQ_OPR_FINFO);

		st->cmp = &tc->cmp_proc_finfo;
		st->eq = &tc->eq_opr_finfo;
	}
	if (g->sorted || g->ordered)
		gen_sort_begin(st);
	if (g->first)
	{
		st->has_final = false;
		gen_first_begin(st);
	}
	return st;
}

/* the generic aggregates of the spec, ready to run (the scan's context, after the expression context exists) */
static void
gen_open(AggExec *ex)
{
	SiftAggSpec *s = ex->spec;
	MemoryContext old = MemoryContextSwitchTo(ex->cxt);
	int			leader = -1;

	ex->gst = palloc0(Max(s->naggs, 1) * sizeof(GenSt *));
	for (int ai = 0; ai < s->naggs; ai++)
		if (s->agg[ai].kind == SAA_GEN)
		{
			if (ex->gas == NULL)
			{
				ex->gas = makeNode(AggState);
				ex->gas->tmpcontext = ex->lutcx;
				ex->gres = AllocSetContextCreate(ex->cxt, "sift agg results", ALLOCSET_DEFAULT_SIZES);
			}
			ex->gst[ai] = gen_new(ex, &s->agg[ai], ai);
			ex->gst[ai]->follower = s->agg[ai].gen->first && leader >= 0;
			if (s->agg[ai].gen->first && leader < 0)
				leader = ai;
		}
	MemoryContextSwitchTo(old);
}

static void
gen_close(AggExec *ex)
{
	for (int ai = 0; ex->gst && ai < ex->spec->naggs; ai++)
		if (ex->gst[ai])
		{
			if (ex->gst[ai]->sort)
				tuplesort_end(ex->gst[ai]->sort);
			FreeExprContext(ex->gst[ai]->cx, true);
		}
}

/* a leaf value as a Datum of the leaf's type, in the current memory context */
static Datum
gen_leaf_value(const SiftSrcLeaf *l, const SiftSrcVec *v, uint32 i, bool *isnull)
{
	*isnull = v->isnull && v->isnull[i];
	if (*isnull)
		return (Datum) 0;
	switch (l->vtype)
	{
		case SCV_I32:
			return leaf_datum(l->typid, ((const int32 *) v->data)[i]);
		case SCV_I64:
			if (l->typid == NUMERICOID && v->ovf && sift_colsrc_bit(v->ovf, i))
				return scaled_numeric(SIFT_NX_NAN, 0);
			if (l->typid == NUMERICOID)
				return NumericGetDatum(int64_div_fast_to_numeric(((const int64 *) v->data)[i], l->scale));
			return Int64GetDatum(((const int64 *) v->data)[i]);
		case SCV_F64:
			return l->typid == FLOAT4OID ? Float4GetDatum((float4) ((const double *) v->data)[i]) :
				Float8GetDatum(((const double *) v->data)[i]);
		case SCV_CODE:
			{
				uint32		code = ((const uint32 *) v->data)[i];

				if (l->wide)
					return sift_aggwide_datum(l->typid, v->dict->bytes + v->dict->off[code], v->dict->off[code + 1] - v->dict->off[code]);
				return PointerGetDatum(cstring_to_text_with_len(v->dict->bytes + v->dict->off[code],
																v->dict->off[code + 1] - v->dict->off[code]));
			}
	}
	return (Datum) 0;
}

/* argument k of tuple j */
static void
gen_arg(AggExec *ex, GenSt *st, const GenIn *in, int j, int k, Datum *arg, bool *isnull)
{
	const SiftAggGenArg *ar = &st->g->arg[k];

	if (ar->mapped)
	{
		uint32		id = in->cval[k][(size_t) in->cout[k][j] * ex->spec->combo[ar->combo].nslots + ar->slot];

		*isnull = id == 0;
		*arg = id ? ex->aot[st->ai].val[id - 1] : (Datum) 0;
	}
	else if (ar->nleaves == 0)
	{
		*arg = st->argk[k];
		*isnull = st->argknull[k];
	}
	else if (ar->expr == NULL)
		*arg = gen_leaf_value(&ex->lay->leaf[ar->leaf[0]], in->vec[k][0], in->idx[k][0][j], isnull);
	else
	{
		for (int p = 0; p < ar->nleaves; p++)
		{
			bool		pnull;
			Datum		d = gen_leaf_value(&ex->lay->leaf[ar->leaf[p]], in->vec[k][p], in->idx[k][p][j], &pnull);

			bind_param(ex, p, d, pnull);
		}
		*arg = ExecEvalExprSwitchContext(st->argx[k], ex->lutcx, isnull);
	}
}

/* the aggregated arguments of tuple j */
static void
gen_args(AggExec *ex, GenSt *st, const GenIn *in, int j, Datum *args, bool *nulls)
{
	for (int k = 0; k < st->g->nargs; k++)
		gen_arg(ex, st, in, j, k, &args[k], &nulls[k]);
}

/* initialize_aggregate */
static void
gen_state_init(GenSt *st, AggAcc *acc, uint32 g)
{
	acc->nf[g] = GEN_INIT;
	if (st->initnull)
		acc->nf[g] |= GEN_NULL | (st->strict ? GEN_NOVAL : 0);
	else
	{
		MemoryContext old = MemoryContextSwitchTo(st->cx->ecxt_per_tuple_memory);

		acc->v[g] = datumCopy(st->initval, st->tbyval, st->tlen);
		MemoryContextSwitchTo(old);
	}
}

/* ExecAggCopyTransValue: a new pass-by-reference state is copied into the aggregate's memory, the old one freed */
static void
gen_keep_state(GenSt *st, AggAcc *acc, uint32 g, uint8 f, Datum ov, Datum nv, bool nvnull)
{
	MemoryContext old;

	if (!st->tbyval && DatumGetPointer(nv) != DatumGetPointer(ov))
	{
		if (!nvnull)
		{
			old = MemoryContextSwitchTo(st->cx->ecxt_per_tuple_memory);
			if (!(DatumIsReadWriteExpandedObject(nv, false, st->tlen) &&
				  MemoryContextGetParent(DatumGetEOHP(nv)->eoh_context) == CurrentMemoryContext))
				nv = datumCopy(nv, st->tbyval, st->tlen);
			MemoryContextSwitchTo(old);
		}
		if (!(f & GEN_NULL))
		{
			if (DatumIsReadWriteExpandedObject(ov, false, st->tlen))
				DeleteExpandedObject(ov);
			else
				pfree(DatumGetPointer(ov));
		}
	}
	acc->v[g] = nv;
	acc->nf[g] = (f & ~(GEN_NULL | GEN_NOVAL)) | (nvnull ? GEN_NULL : 0);
}

/* advance_transition_function: the transition function on one input; its temporary memory is the scan's per-tuple memory */
static void
gen_advance(AggExec *ex, GenSt *st, AggAcc *acc, uint32 g, const Datum *args, const bool *nulls)
{
	FunctionCallInfo fc = st->tfc;
	int			nargs = st->g->ntrans;
	uint8		f;
	Datum		nv,
				ov;
	MemoryContext old;

	if (!(acc->nf[g] & GEN_INIT))
		gen_state_init(st, acc, g);
	f = acc->nf[g];
	if (st->strict)
	{
		for (int k = 0; k < nargs; k++)
			if (nulls[k])
				return;
		if (f & GEN_NOVAL)
		{
			old = MemoryContextSwitchTo(st->cx->ecxt_per_tuple_memory);
			acc->v[g] = datumCopy(args[0], st->tbyval, st->tlen);
			MemoryContextSwitchTo(old);
			acc->nf[g] = f & ~(GEN_NOVAL | GEN_NULL);
			return;
		}
		if (f & GEN_NULL)
			return;
	}
	ov = acc->v[g];
	fc->args[0].value = ov;
	fc->args[0].isnull = (f & GEN_NULL) != 0;
	for (int k = 0; k < nargs; k++)
	{
		fc->args[k + 1].value = args[k];
		fc->args[k + 1].isnull = nulls[k];
	}
	fc->isnull = false;
	ex->gas->curpertrans = &st->pertrans;
	ex->gas->curperagg = NULL;
	ex->gas->curaggcontext = st->cx;
	old = MemoryContextSwitchTo(ex->lutcx->ecxt_per_tuple_memory);
	nv = FunctionCallInvoke(fc);
	MemoryContextSwitchTo(old);
	gen_keep_state(st, acc, g, f, ov, nv, fc->isnull);
}

/* advance_combine_function: the partial state nv of a worker joins the state of group g */
static void
gen_combine(AggExec *ex, GenSt *st, AggAcc *acc, uint32 g, Datum nv)
{
	FunctionCallInfo fc = st->cfc;
	uint8		f;
	Datum		ov,
				res;
	MemoryContext old;

	if (!(acc->nf[g] & GEN_INIT))
		gen_state_init(st, acc, g);
	f = acc->nf[g];
	if (st->combinefn.fn_strict && (f & GEN_NULL))
	{
		old = MemoryContextSwitchTo(st->cx->ecxt_per_tuple_memory);
		acc->v[g] = datumCopy(nv, st->tbyval, st->tlen);
		MemoryContextSwitchTo(old);
		acc->nf[g] = f & ~(GEN_NULL | GEN_NOVAL);
		return;
	}
	ov = acc->v[g];
	fc->args[0].value = ov;
	fc->args[0].isnull = (f & GEN_NULL) != 0;
	fc->args[1].value = nv;
	fc->args[1].isnull = false;
	fc->isnull = false;
	ex->gas->curpertrans = &st->pertrans;
	ex->gas->curperagg = NULL;
	ex->gas->curaggcontext = st->cx;
	old = MemoryContextSwitchTo(ex->lutcx->ecxt_per_tuple_memory);
	res = FunctionCallInvoke(fc);
	MemoryContextSwitchTo(old);
	gen_keep_state(st, acc, g, f, ov, res, fc->isnull);
}

static void gen_first_put(GenSt *st, AggAcc *acc, uint32 g, StringInfo vb);
static void gen_first_merge(GenSt *st, AggAcc *acc, uint32 g, const char *at);

/* a worker's partial state of group g, appended to vb as a length (0: none) and bytes: the serial function's, else the datum's */
static void
gen_state_put(AggExec *ex, GenSt *st, AggAcc *acc, uint32 g, StringInfo vb)
{
	uint8		f = acc->nf[g];
	uint32		len = 0;

	if (st->g->first)
	{
		gen_first_put(st, acc, g, vb);
		return;
	}
	if (!(f & GEN_INIT) || (f & GEN_NULL))
	{
		appendBinaryStringInfo(vb, (char *) &len, sizeof(len));
		return;
	}
	if (st->sfc)
	{
		bytea	   *r;

		st->sfc->args[0].value = acc->v[g];
		st->sfc->args[0].isnull = false;
		st->sfc->isnull = false;
		ex->gas->curpertrans = &st->pertrans;
		ex->gas->curperagg = NULL;
		ex->gas->curaggcontext = st->cx;
		r = DatumGetByteaPP(FunctionCallInvoke(st->sfc));
		len = VARSIZE_ANY(r);
		appendBinaryStringInfo(vb, (char *) &len, sizeof(len));
		appendBinaryStringInfo(vb, (char *) r, len);
	}
	else
	{
		Size		sz = datumEstimateSpace(acc->v[g], false, st->tbyval, st->tlen);
		char	   *at;

		len = sz;
		appendBinaryStringInfo(vb, (char *) &len, sizeof(len));
		enlargeStringInfo(vb, sz);
		at = vb->data + vb->len;
		datumSerialize(acc->v[g], false, st->tbyval, st->tlen, &at);
		vb->len += sz;
	}
}

/* the next partial state of the blob (the layout of gen_state_put) joins group g */
static void
gen_state_merge(AggExec *ex, GenSt *st, AggAcc *acc, uint32 g, const char **cur)
{
	uint32		len;
	Datum		nv;
	MemoryContext old;

	memcpy(&len, *cur, sizeof(len));
	*cur += sizeof(len);
	if (len == 0)
		return;
	if (st->g->first)
	{
		old = MemoryContextSwitchTo(ex->lutcx->ecxt_per_tuple_memory);
		gen_first_merge(st, acc, g, *cur);
		MemoryContextSwitchTo(old);
		*cur += len;
		ResetExprContext(ex->lutcx);
		return;
	}
	old = MemoryContextSwitchTo(ex->lutcx->ecxt_per_tuple_memory);
	if (st->dfc)
	{
		bytea	   *r = palloc(len);

		memcpy(r, *cur, len);
		st->dfc->args[0].value = PointerGetDatum(r);
		st->dfc->args[0].isnull = false;
		st->dfc->args[1].value = (Datum) 0;
		st->dfc->args[1].isnull = false;
		st->dfc->isnull = false;
		ex->gas->curpertrans = &st->pertrans;
		ex->gas->curperagg = NULL;
		ex->gas->curaggcontext = st->cx;
		nv = FunctionCallInvoke(st->dfc);
	}
	else
	{
		const char *at = *cur;
		bool		isnull;

		nv = datumRestore((char **) &at, &isnull);
	}
	*cur += len;
	MemoryContextSwitchTo(old);
	gen_combine(ex, st, acc, g, nv);
	ResetExprContext(ex->lutcx);
}

/* finalize_aggregate, once per group; the result lives as long as the scan */
static void
gen_final(AggExec *ex, GenSt *st, AggAcc *acc, uint32 g)
{
	uint8		f;
	Datum		res;
	bool		rnull;
	MemoryContext old;

	if (!(acc->nf[g] & GEN_INIT))
		gen_state_init(st, acc, g);
	f = acc->nf[g];
	old = MemoryContextSwitchTo(ex->gres);
	if (!st->has_final)
	{
		res = acc->v[g];
		rnull = (f & GEN_NULL) != 0;
	}
	else
	{
		FunctionCallInfo fc = st->ffc;
		bool		anynull = (f & GEN_NULL) != 0;

		ex->gas->curperagg = &st->peragg;
		ex->gas->curpertrans = NULL;
		ex->gas->curaggcontext = st->cx;
		fc->args[0].value = MakeExpandedObjectReadOnly(acc->v[g], anynull, st->tlen);
		fc->args[0].isnull = anynull;
		for (int i = 1; i < st->nfinal; i++)
		{
			fc->args[i].value = i <= st->ndirect ? st->dir[i - 1] : (Datum) 0;
			fc->args[i].isnull = i <= st->ndirect ? st->dirnull[i - 1] : true;
			anynull |= fc->args[i].isnull;
		}
		if (st->finalfn.fn_strict && anynull)
		{
			res = (Datum) 0;
			rnull = true;
		}
		else
		{
			fc->isnull = false;
			res = FunctionCallInvoke(fc);
			rnull = fc->isnull;
		}
	}
	if (!rnull && !st->rbyval)
		res = datumCopy(res, st->rbyval, st->rlen);
	MemoryContextSwitchTo(old);
	acc->r[g] = res;
	acc->nf[g] = f | GEN_DONE | (rnull ? GEN_RNULL : 0);
}

/* what the distinct values of a DISTINCT aggregate hold: the pair set, the value table and the values themselves */
static uint64
gen_hash_bytes(const DSet *d, const OutTable *ot)
{
	return d->cap * sizeof(uint64) + ot->cap * (sizeof(Datum) + sizeof(uint32)) + (uint64) ot->nslot * sizeof(uint32) + ot->bytes;
}

/*
 * The distinct pairs outgrew work_mem: they move to a tuplesort, where the inputs that follow go too and PostgreSQL's own
 * sort-and-skip-equal of a DISTINCT aggregate removes the duplicates (spilling to disk when it must).
 */
static void
gen_spill(AggExec *ex, int ai)
{
	GenSt	   *st = ex->gst[ai];
	DSet	   *d = &ex->dset[ai];
	OutTable   *ot = &ex->aot[ai];
	AggAcc	   *acc = &ex->acc[ai];
	Datum		args[SIFT_AGG_MAX_GARGS] = {0};
	bool		nulls[SIFT_AGG_MAX_GARGS] = {false};
	MemoryContext old = MemoryContextSwitchTo(ex->cxt);

	elog(DEBUG1, "sift: DISTINCT of %s moved to a tuplesort", get_func_name(st->aggref->aggfnoid));
	gen_sort_begin(st);
	MemoryContextSwitchTo(old);
	st->spilled = true;
	for (uint64 i = 0; i < d->cap; i++)
		if (d->slot[i])
		{
			uint32		id = (uint32) d->slot[i];

			args[st->g->darg] = id ? ot->val[id - 1] : (Datum) 0;
			nulls[st->g->darg] = id == 0;
			gen_sort_put(st, acc, (d->slot[i] >> 32) & 0x7fffffff, args, nulls);
		}
	if (!ot->byval)
		for (uint32 i = 0; i < ot->n; i++)
			pfree(DatumGetPointer(ot->val[i]));
	pfree(ot->val);
	pfree(ot->hv);
	pfree(ot->slot);
	pfree(d->slot);
	ot->n = ot->cap = ot->nslot = 0;
	ot->val = NULL;
	ot->hv = NULL;
	ot->slot = NULL;
	ot->bytes = 0;
	d->slot = NULL;
	d->n = d->cap = 0;
}

/* ---------- DISTINCT ON: the best row of each group ---------- */

/* does an argument go into the best row (a constant is the same in every row) */
static bool
gen_stored(const SiftAggGen *g, int k)
{
	return g->arg[k].nleaves > 0 || g->arg[k].mapped;
}

static void
gen_best_free(GenSt *st, GenBest *b)
{
	int			n = st->g->nargs;

	for (int k = 0; k < n; k++)
		if (gen_stored(st->g, k) && !GEN_BEST_NULL(b, n, k) && !st->abyval[k])
			pfree(DatumGetPointer(b[k]));
	pfree(b);
}

/* group g keeps the row whose arguments these are, in memory that lives as long as the scan; the row it had is freed */
static void
gen_best_set(GenSt *st, AggAcc *acc, uint32 g, const Datum *args, const bool *nulls)
{
	int			n = st->g->nargs;
	MemoryContext old = MemoryContextSwitchTo(st->cx->ecxt_per_tuple_memory);
	GenBest    *nb = palloc0(GEN_BEST_SIZE(n)),
			   *ob = (GenBest *) DatumGetPointer(acc->v[g]);

	for (int k = 0; k < n; k++)
		if (gen_stored(st->g, k))
		{
			GEN_BEST_NULL(nb, n, k) = nulls[k];
			nb[k] = nulls[k] ? (Datum) 0 : datumCopy(args[k], st->abyval[k], st->alen[k]);
		}
	if (ob != NULL)
		gen_best_free(st, ob);
	acc->v[g] = PointerGetDatum(nb);
	MemoryContextSwitchTo(old);
}

/* the sort keys of a row against those of the best row: < 0 when the row comes first */
static int
gen_first_cmp(GenSt *st, const Datum *args, const bool *nulls, const GenBest *b)
{
	int			n = st->g->nargs;

	for (int i = 0; i < st->nfk; i++)
	{
		int			k = st->fkey[i],
					c;
		Datum		bv = gen_stored(st->g, k) ? b[k] : st->argk[k];
		bool		bn = gen_stored(st->g, k) ? GEN_BEST_NULL(b, n, k) : st->argknull[k];

		if ((c = ApplySortComparator(args[k], nulls[k], bv, bn, &st->fsup[i])) != 0)
			return c;
	}
	return 0;
}

/* tuple j of group gid: its row is the group's best when its sort keys come first (the values of the other arguments are read only then) */
static bool
gen_first(AggExec *ex, GenSt *st, AggAcc *acc, const GenIn *in, int j, uint32 gid)
{
	GenBest    *b = (GenBest *) DatumGetPointer(acc->v[gid]);
	Datum		args[SIFT_AGG_MAX_GARGS];
	bool		nulls[SIFT_AGG_MAX_GARGS];

	for (int i = 0; i < st->nfk; i++)
		gen_arg(ex, st, in, j, st->fkey[i], &args[st->fkey[i]], &nulls[st->fkey[i]]);
	if (b != NULL && gen_first_cmp(st, args, nulls, b) >= 0)
		return false;
	for (int k = 0; k < st->g->nargs; k++)
		if (!st->iskey[k])
			gen_arg(ex, st, in, j, k, &args[k], &nulls[k]);
	gen_best_set(st, acc, gid, args, nulls);
	return true;
}

/* the scan has ended: the first argument of the best row of each group is the group's value (the carrier aggregate has no transition or final function to run) */
static void
gen_flush_first(AggExec *ex, int ai)
{
	GenSt	   *st = ex->gst[ai];
	AggAcc	   *acc = &ex->acc[ai];
	uint32		ngr = ex->hash ? ex->ngroups : ex->gcap;
	int			n = st->g->nargs;

	for (uint32 g = 0; g < ngr; g++)
	{
		GenBest    *b = (GenBest *) DatumGetPointer(acc->v[g]);

		if (b == NULL)
			continue;
		acc->v[g] = b[0];
		acc->nf[g] = GEN_INIT | (GEN_BEST_NULL(b, n, 0) ? GEN_NULL : 0);
		for (int k = 1; k < n; k++)
			if (gen_stored(st->g, k) && !GEN_BEST_NULL(b, n, k) && !st->abyval[k])
				pfree(DatumGetPointer(b[k]));
		pfree(b);
	}
}

/* a worker's best row of group g, appended to vb as a length (0: none) and the serialized values */
static void
gen_first_put(GenSt *st, AggAcc *acc, uint32 g, StringInfo vb)
{
	const GenBest *b = (const GenBest *) DatumGetPointer(acc->v[g]);
	int			n = st->g->nargs;
	uint32		len = 0;
	char	   *at;

	if (b != NULL)
		for (int k = 0; k < n; k++)
			if (gen_stored(st->g, k))
				len += datumEstimateSpace(b[k], GEN_BEST_NULL(b, n, k), st->abyval[k], st->alen[k]);
	appendBinaryStringInfo(vb, (char *) &len, sizeof(len));
	if (len == 0)
		return;
	enlargeStringInfo(vb, len);
	at = vb->data + vb->len;
	for (int k = 0; k < n; k++)
		if (gen_stored(st->g, k))
			datumSerialize(b[k], GEN_BEST_NULL(b, n, k), st->abyval[k], st->alen[k], &at);
	vb->len += len;
}

/* the best row a worker serialized joins group g's: it replaces the row of the leader when its sort keys come first */
static void
gen_first_merge(GenSt *st, AggAcc *acc, uint32 g, const char *at)
{
	GenBest    *b = (GenBest *) DatumGetPointer(acc->v[g]);
	Datum		args[SIFT_AGG_MAX_GARGS];
	bool		nulls[SIFT_AGG_MAX_GARGS];

	for (int k = 0; k < st->g->nargs; k++)
		if (gen_stored(st->g, k))
			args[k] = datumRestore((char **) &at, &nulls[k]);
		else
		{
			args[k] = st->argk[k];
			nulls[k] = st->argknull[k];
		}
	if (b == NULL || gen_first_cmp(st, args, nulls, b) < 0)
		gen_best_set(st, acc, g, args, nulls);
}

/* the tuples of the chunk: DISTINCT collects the pairs, WITHIN GROUP sorts the values by group, the others advance the states */
void
gen_update(AggExec *ex, int ai, const uint64 *ok, int nsel)
{
	GenSt	   *st = ex->gst[ai];
	const SiftAggGen *g = st->g;
	AggAcc	   *acc = &ex->acc[ai];
	GenIn		in;
	Datum		args[SIFT_AGG_MAX_GARGS];
	bool		nulls[SIFT_AGG_MAX_GARGS];
	MemoryContext old = MemoryContextSwitchTo(ex->lutcx->ecxt_per_tuple_memory);

	for (int k = 0; k < g->nargs; k++)
		if (g->arg[k].mapped)
		{
			in.cout[k] = combo_outs(ex, g->arg[k].combo);
			in.cval[k] = ex->cs[g->arg[k].combo].val;
		}
	for (int k = 0; k < g->nargs; k++)
		for (int p = 0; p < g->arg[k].nleaves; p++)
		{
			in.vec[k][p] = get_vec(ex, g->arg[k].leaf[p]);
			gather_idx(ex, g->arg[k].leaf[p], nsel, &in.idx[k][p]);
		}
	for (int j = 0; j < nsel; j++)
	{
		uint32		gid = ex->gid[j];

		if (ok && !sift_colsrc_bit(ok, j))
		{
			ex->fwin[j] = 0;
			continue;
		}
		if (g->first)
		{
			if (!st->follower)
				ex->fwin[j] = gen_first(ex, st, acc, &in, j, gid);
			else if (ex->fwin[j])
			{
				gen_args(ex, st, &in, j, args, nulls);
				gen_best_set(st, acc, gid, args, nulls);
			}
			ResetExprContext(ex->lutcx);
			continue;
		}
		gen_args(ex, st, &in, j, args, nulls);
		if (g->sorted || st->spilled)
			gen_sort_put(st, acc, gid, args, nulls);
		else if (g->ordered)
		{
			/* percentile_*, mode(): the transition function leaves NULLs out; a hypothetical-set aggregate counts them */
			if (g->hyp || !nulls[0])
				gen_sort_put(st, acc, gid, args, nulls);
		}
		else if (g->distinct)
		{
			bool		dn = nulls[g->darg];

			if (!(st->strict && memchr(nulls, true, g->nargs)))
			{
				uint32		id = 0;

				if (!dn)
				{
					MemoryContextSwitchTo(ex->tcxt);
					id = ot_id(&ex->aot[ai], args[g->darg]);
					MemoryContextSwitchTo(ex->lutcx->ecxt_per_tuple_memory);
				}
				dset_add(ex, &ex->dset[ai], gid, id);
			}
		}
		else
			gen_advance(ex, st, acc, gid, args, nulls);
		ResetExprContext(ex->lutcx);
	}
	MemoryContextSwitchTo(old);
	if (g->distinct && !g->sorted && !st->spilled && gen_hash_bytes(&ex->dset[ai], &ex->aot[ai]) > (uint64) work_mem * 1024)
		gen_spill(ex, ai);
}

typedef struct GenPair
{
	uint32		g;
	uint32		id;
} GenPair;

typedef struct GenSortArg
{
	GenSt	   *st;
	const OutTable *ot;
	Oid			collid;
} GenSortArg;

/* by group, then the type's btree order (NULL last), then the id: PostgreSQL sorts a DISTINCT aggregate's input this way */
static int
gen_pair_cmp(const void *pa, const void *pb, void *arg)
{
	const GenPair *a = pa,
			   *b = pb;
	const GenSortArg *sa = arg;
	int32		c;

	if (a->g != b->g)
		return a->g < b->g ? -1 : 1;
	if (a->id == 0 || b->id == 0)
		return a->id == b->id ? 0 : a->id == 0 ? 1 : -1;
	c = DatumGetInt32(FunctionCall2Coll(sa->st->cmp, sa->collid, sa->ot->val[a->id - 1], sa->ot->val[b->id - 1]));
	if (c != 0)
		return c;
	return a->id < b->id ? -1 : 1;
}

/* DISTINCT: the distinct values of each group go through the transition function, in order, the first of a run of equal ones only */
static void
gen_flush_distinct(AggExec *ex, int ai)
{
	GenSt	   *st = ex->gst[ai];
	const DSet *d = &ex->dset[ai];
	AggAcc	   *acc = &ex->acc[ai];
	GenSortArg	sa = {st, &ex->aot[ai], ((Aggref *) st->g->aggref)->inputcollid};
	GenPair    *pr = palloc(Max(d->n, 1) * sizeof(GenPair));
	uint64		n = 0;
	Datum		args[SIFT_AGG_MAX_GARGS];
	bool		nulls[SIFT_AGG_MAX_GARGS];
	int			darg = st->g->darg;

	for (int k = 0; k < st->g->nargs; k++)
	{
		args[k] = st->argk[k];
		nulls[k] = st->argknull[k];
	}
	for (uint64 i = 0; i < d->cap; i++)
		if (d->slot[i])
			pr[n++] = (GenPair) {(d->slot[i] >> 32) & 0x7fffffff, (uint32) d->slot[i]};
	qsort_arg(pr, n, sizeof(GenPair), gen_pair_cmp, &sa);
	for (uint64 i = 0; i < n; i++)
	{
		Datum		v = pr[i].id ? ex->aot[ai].val[pr[i].id - 1] : (Datum) 0;

		if (i > 0 && pr[i - 1].g == pr[i].g)
		{
			if (pr[i].id == 0 && pr[i - 1].id == 0)
				continue;
			if (pr[i].id != 0 && pr[i - 1].id != 0 &&
				DatumGetBool(FunctionCall2Coll(st->eq, sa.collid, ex->aot[ai].val[pr[i - 1].id - 1], v)))
				continue;
		}
		args[darg] = v;
		nulls[darg] = pr[i].id == 0;
		gen_advance(ex, st, acc, pr[i].g, args, nulls);
		ResetExprContext(ex->lutcx);
	}
	pfree(pr);
}

/*
 * DISTINCT in the sort: is the tuple just read equal, in every column the DISTINCT list names (NULLs equal), to the one before it of the
 * same group? The tuple becomes the one before when it is not.
 */
static bool
gen_same_input(GenSt *st, bool samegroup)
{
	bool		same = samegroup;
	MemoryContext old;

	for (int i = 0; same && i < st->ndcol; i++)
	{
		bool		n;
		Datum		d = slot_getattr(st->get, 2 + st->dcol[i], &n);

		same = n == st->prevnull[i] && (n || DatumGetBool(FunctionCall2Coll(&st->deq[i], ((Aggref *) st->g->aggref)->inputcollid,
																		  st->prev[i], d)));
	}
	if (same)
		return true;
	MemoryContextReset(st->pcx);
	old = MemoryContextSwitchTo(st->pcx);
	for (int i = 0; i < st->ndcol; i++)
	{
		Datum		d = slot_getattr(st->get, 2 + st->dcol[i], &st->prevnull[i]);

		st->prev[i] = st->prevnull[i] ? (Datum) 0 : datumCopy(d, st->dbyval[i], st->dlen[i]);
	}
	MemoryContextSwitchTo(old);
	return false;
}

/* WITHIN GROUP, ORDER BY: the values of a group go through the transition function, then the final function; the state is freed */
static void
gen_flush_ordered(AggExec *ex, int ai)
{
	GenSt	   *st = ex->gst[ai];
	AggAcc	   *acc = &ex->acc[ai];
	uint32		cur = UINT32_MAX,
				ngr = ex->hash ? ex->ngroups : ex->gcap,
			   *sid2g = palloc(Max(st->nsid, 1) * sizeof(uint32));
	bool		isnull,
				nulls[SIFT_AGG_MAX_GARGS] = {false};
	Datum		args[SIFT_AGG_MAX_GARGS];

	for (uint32 g = 0; g < ngr; g++)
		if (acc->i[g])
			sid2g[acc->i[g] - 1] = g;
	tuplesort_performsort(st->sort);
	while (tuplesort_gettupleslot(st->sort, true, false, st->get, NULL))
	{
		uint32		g = sid2g[DatumGetInt32(slot_getattr(st->get, 1, &isnull))];

		if (st->tmode)
			for (int k = 0; k < st->g->ntrans; k++)
			{
				bool		cn = st->vcol[k] < 0;

				args[k] = cn ? st->argk[k] : slot_getattr(st->get, 2 + st->vcol[k], &isnull);
				nulls[k] = cn ? st->argknull[k] : isnull;
			}
		else
			args[0] = slot_getattr(st->get, 2, &isnull);
		if (g != cur && cur != UINT32_MAX)
		{
			gen_final(ex, st, acc, cur);
			acc->v[cur] = (Datum) 0;
			ReScanExprContext(st->cx);
		}
		if (st->g->distinct && gen_same_input(st, g == cur))
			continue;
		cur = g;
		gen_advance(ex, st, acc, g, args, nulls);
		ResetExprContext(ex->lutcx);
	}
	if (cur != UINT32_MAX)
	{
		gen_final(ex, st, acc, cur);
		acc->v[cur] = (Datum) 0;
		ReScanExprContext(st->cx);
	}
	tuplesort_end(st->sort);
	st->sort = NULL;
	pfree(sid2g);
}

/* the scan has ended: the aggregates that kept their values until now give them to the transition functions */
static void
gen_flush(AggExec *ex)
{
	for (int ai = 0; ex->gst && ai < ex->spec->naggs; ai++)
		if (ex->gst[ai] && ex->gst[ai]->g->first)
			gen_flush_first(ex, ai);
		else if (ex->gst[ai] && ex->gst[ai]->tmode)
			gen_flush_ordered(ex, ai);
		else if (ex->gst[ai] && ex->gst[ai]->g->distinct)
			gen_flush_distinct(ex, ai);
		else if (ex->gst[ai] && ex->gst[ai]->g->ordered)
			gen_flush_ordered(ex, ai);
}

/* the final function of every group that is listed and was not finished */
static void
gen_finish(AggExec *ex)
{
	for (int ai = 0; ex->gst && ai < ex->spec->naggs; ai++)
		if (ex->gst[ai])
			for (uint32 i = 0; i < ex->nglist; i++)
				if (!(ex->acc[ai].nf[ex->glist[i]] & GEN_DONE))
					gen_final(ex, ex->gst[ai], &ex->acc[ai], ex->glist[i]);
}

static void
semi_lut_build(AggExec *ex, int ci)
{
	SiftAggCombo *cb = &ex->spec->combo[ci];
	const SiftAggJoin *j = &ex->spec->join[cb->join];
	ComboSt    *cs = &ex->cs[ci];
	const MapTab *m = cs->map;
	int64		lo = PG_INT32_MAX,
				hi = PG_INT32_MIN;
	bool	   *keep;

	if ((j->kind != SJ_SEMI && j->kind != SJ_ANTI) || j->nkey != 1 || j->keypos[0] < 0 || cb->nleaves != 1 || m->ktext[0] ||
		cb->leaf[0] < 0 || cb->leaf[0] >= ex->nbase || ex->lay->leaf[cb->leaf[0]].vtype != SCV_I32 || !sift_agg_semi_lut ||
		cb->nslots != (j->fslot >= 0 ? 1 : 0) || (j->fslot >= 0 && slot_reads_leaf(cb, j->fslot)))
		return;
	/* the rest of the ON clause (the table's own conditions) holds or not per row of the table: once, not per probe; a key outside int32 matches no leaf */
	keep = palloc(Max(m->nrow, 1) * sizeof(bool));
	for (uint32 r = 0; r < m->nrow; r++)
	{
		keep[r] = m->kint[r] >= PG_INT32_MIN && m->kint[r] <= PG_INT32_MAX;
		if (keep[r] && j->fslot >= 0 && !m->filtered)
		{
			bool		isnull;
			Datum		d;

			for (int c = 0; c < j->ncol; c++)
				bind_param(ex, cb->nleaves + c, m->col[(size_t) r * m->ncol + c], m->cnull[(size_t) r * m->ncol + c]);
			d = ExecEvalExprSwitchContext(cs->slot[j->fslot], ex->lutcx, &isnull);
			keep[r] = !isnull && DatumGetBool(d);
			if ((r & 1023) == 1023)
				ResetExprContext(ex->lutcx);
		}
		if (keep[r])
		{
			lo = Min(lo, m->kint[r]);
			hi = Max(hi, m->kint[r]);
		}
	}
	ResetExprContext(ex->lutcx);
	if (lo <= hi && hi - lo + 1 > (int64) sift_agg_semi_lut_size * 1024)
	{
		pfree(keep);
		return;
	}
	cs->slo = lo <= hi ? (int32) lo : 0;
	cs->snlut = lo <= hi ? (uint32) (hi - lo + 1) : 0;
	cs->slut = MemoryContextAllocZero(ex->cxt, cs->snlut + SIFT_AGG_PAD);
	for (uint32 r = 0; r < m->nrow; r++)
		if (keep[r])
			cs->slut[m->kint[r] - cs->slo] = 1;
	pfree(keep);
	ex->nsemi++;
	ex->semi_bytes += cs->snlut;
}

/* the tuples of `in` that pass a semi (the key is in the table) or anti (it is not, or NULL) join of the table, to the next stage */

/* ---------- two level: per-row aggregates under an outer aggregate ---------- */

/* the cells of every arm to newcap groups, renumbered through remap (NULL: as they are); the touched list follows */
void
arms_resize(AggExec *ex, uint32 newcap, const uint32 *remap, uint32 oldcap)
{
	SiftAggSpec *s = ex->spec;

	for (int k = 0; k < s->narms; k++)
	{
		ArmSt	   *a = &ex->arm[k];
		SiftAggArm *sa = &s->arm[k];
		uint64	   *nord = MemoryContextAlloc(ex->tcxt, newcap * sizeof(uint64));

		memset(nord, 0xff, newcap * sizeof(uint64));
		for (int ai = 0; ai < sa->ninner; ai++)
		{
			int64	   *nn = MemoryContextAllocZero(ex->tcxt, newcap * sizeof(int64)),
					   *ni = MemoryContextAllocZero(ex->tcxt, newcap * sizeof(int64));
			double	   *nd = MemoryContextAllocZero(ex->tcxt, newcap * sizeof(double));
			uint8	   *nnf = MemoryContextAllocZero(ex->tcxt, newcap);

			for (uint32 g = 0; g < oldcap && a->ord; g++)
			{
				uint32		t = remap ? remap[g] : g;

				if (t == UINT32_MAX || a->ord[g] == UINT64_MAX)
					continue;
				nn[t] = a->n[ai][g];
				ni[t] = a->i[ai][g];
				nd[t] = a->d[ai][g];
				nnf[t] = a->nf[ai][g];
			}
			a->n[ai] = nn;
			a->i[ai] = ni;
			a->d[ai] = nd;
			a->nf[ai] = nnf;
		}
		for (uint32 g = 0; g < oldcap && a->ord; g++)
		{
			uint32		t = remap ? remap[g] : g;

			if (t != UINT32_MAX && a->ord[g] != UINT64_MAX)
				nord[t] = a->ord[g];
		}
		a->ord = nord;
	}
	if (remap)
		for (uint32 t = 0; t < ex->ntouch; t++)
			ex->touched[t].g = remap[ex->touched[t].g];
}

static void
arms_init(AggExec *ex)
{
	SiftAggSpec *s = ex->spec;
	int			maxv = VEC + PADW;

	ex->arm = MemoryContextAllocZero(ex->cxt, Max(s->narms, 1) * sizeof(ArmSt));
	ex->arm_mask = ~(uint32) 0;
	ex->cur_order = UINT64_MAX;
	ex->setg = MemoryContextAlloc(ex->cxt, Max(s->nsets, 1) * sizeof(uint32));
	for (int k = 0; k < s->narms; k++)
	{
		ArmSt	   *a = &ex->arm[k];
		int			ni = s->arm[k].ninner;

		a->gid = MemoryContextAlloc(ex->cxt, maxv * sizeof(uint32));
		a->ok = MemoryContextAlloc(ex->cxt, maxv);
		if (s->arm[k].hexpr)
		{
			MemoryContext old = MemoryContextSwitchTo(ex->cxt);

			a->hstate = ExecInitExpr((Expr *) s->arm[k].hexpr, EXPR_PARENT(ex));
			MemoryContextSwitchTo(old);
		}
		a->iok = MemoryContextAlloc(ex->cxt, Max(ni, 1) * sizeof(uint8 *));
		a->dx = MemoryContextAlloc(ex->cxt, Max(ni, 1) * sizeof(double *));
		a->ix = MemoryContextAlloc(ex->cxt, Max(ni, 1) * sizeof(int64 *));
		a->n = MemoryContextAllocZero(ex->cxt, Max(ni, 1) * sizeof(int64 *));
		a->i = MemoryContextAllocZero(ex->cxt, Max(ni, 1) * sizeof(int64 *));
		a->d = MemoryContextAllocZero(ex->cxt, Max(ni, 1) * sizeof(double *));
		a->nf = MemoryContextAllocZero(ex->cxt, Max(ni, 1) * sizeof(uint8 *));
		a->amax = MemoryContextAllocZero(ex->cxt, Max(ni, 1) * sizeof(double));
		for (int ai = 0; ai < ni; ai++)
		{
			a->iok[ai] = MemoryContextAlloc(ex->cxt, maxv);
			a->dx[ai] = MemoryContextAlloc(ex->cxt, maxv * sizeof(double));
			a->ix[ai] = MemoryContextAlloc(ex->cxt, maxv * sizeof(int64));
		}
	}
	ex->captouch = 1024;
	ex->touched = MemoryContextAlloc(ex->cxt, ex->captouch * sizeof(Touch));
}

/* the inner aggregate ai of arm k for the tuples of the chunk: the ones it takes and their values */
static void
arm_inner_prepare(AggExec *ex, ArmSt *a, SiftAggAgg *ia, int ai, int n)
{
	bool		all;
	const uint32 *idx,
			   *out;
	const SiftSrcVec *v;

	agg_selection(ex, ia, ex->cstart, ex->cn, n, &all);
	for (int j = 0; j < n; j++)
		a->iok[ai][j] = all || sift_colsrc_bit(ex->okm, j);
	switch (ia->kind)
	{
		case SAA_SUM_INT:
			v = get_vec(ex, ia->leaf);
			gather_idx(ex, ia->leaf, n, &idx);
			for (int j = 0; j < n; j++)
				a->ix[ai][j] = ((const int32 *) v->data)[idx[j]];
			break;
		case SAA_SUM_F64:
		case SAA_AVG_F64:
			if (ia->nvx)
				memcpy(a->dx[ai], eval_vx(ex, ia, n), n * sizeof(double));
			else
			{
				v = get_vec(ex, ia->leaf);
				gather_idx(ex, ia->leaf, n, &idx);
				for (int j = 0; j < n; j++)
					a->dx[ai][j] = ((const double *) v->data)[idx[j]];
			}
			break;
		case SAA_X_SUM:
		case SAA_X_AVG:
			out = combo_outs(ex, ia->combo);
			for (int j = 0; j < n; j++)
			{
				uint32		id = out[j] == COMBO_NONE ? 0 : combo_val(ex, ia->combo, out[j], ia->slot);

				a->iok[ai][j] &= id != 0;
				a->dx[ai][j] = id ? DatumGetFloat8(ex->cs[ia->combo].dat[id - 1]) : 0;
			}
			break;
		default:
			break;
	}
}

static void
cell_start(AggExec *ex, int k, ArmSt *a, uint32 g, uint64 row)
{
	a->ord[g] = row;
	for (int ai = 0; ai < ex->spec->arm[k].ninner; ai++)
	{
		a->n[ai][g] = 0;
		a->i[ai][g] = 0;
		a->d[ai][g] = 0;
		a->nf[ai][g] = 0;
	}
	if (ex->ntouch == ex->captouch)
	{
		ex->captouch *= 2;
		ex->touched = repalloc(ex->touched, ex->captouch * sizeof(Touch));
	}
	ex->touched[ex->ntouch].arm = k;
	ex->touched[ex->ntouch++].g = g;
	ex->seen[g]++;
}

static void
cell_add(ArmSt *a, SiftAggAgg *ia, int ai, uint32 g, int j)
{
	switch (ia->kind)
	{
		case SAA_SUM_INT:
			a->i[ai][g] += a->ix[ai][j];
			break;
		case SAA_SUM_F64:
		case SAA_AVG_F64:
		case SAA_X_SUM:
		case SAA_X_AVG:
			{
				double		x = a->dx[ai][j];

				a->d[ai][g] += x;
				if (!isfinite(x))
					a->nf[ai][g] = 1;
				else
					a->amax[ai] = Max(a->amax[ai], fabs(x));
			}
			break;
		default:
			break;
	}
	a->n[ai][g]++;
}

/* the value of inner aggregate ai in a cell: false for NULL */
static bool
cell_value(ArmSt *a, SiftAggAgg *ia, int ai, uint32 g, double *dv, int64 *iv)
{
	switch (ia->kind)
	{
		case SAA_COUNT_STAR:
		case SAA_COUNT:
			*iv = a->n[ai][g];
			return true;
		case SAA_SUM_INT:
			*iv = a->i[ai][g];
			return a->n[ai][g] > 0;
		case SAA_SUM_F64:
		case SAA_X_SUM:
			if (isinf(a->d[ai][g]) && !a->nf[ai][g])
				float_overflow_error();
			*dv = a->d[ai][g];
			return a->n[ai][g] > 0;
		case SAA_AVG_F64:
		case SAA_X_AVG:
			if (a->n[ai][g] == 0)
				return false;
			*dv = a->d[ai][g] / (double) a->n[ai][g];
			return true;
		default:
			elog(ERROR, "sift: unexpected inner aggregate");
	}
	return false;
}

static bool
cond_holds(const SiftAggCond *c, double dv, int64 iv)
{
	int			cmp = c->isint ? pg_cmp_s64(iv, c->ci) : float8_cmp_internal(dv, c->cd);

	switch ((SiftCmpOp) c->op)
	{
		case SCMP_EQ:
			return cmp == 0;
		case SCMP_NE:
			return cmp != 0;
		case SCMP_LT:
			return cmp < 0;
		case SCMP_LE:
			return cmp <= 0;
		case SCMP_GT:
			return cmp > 0;
		case SCMP_GE:
			return cmp >= 0;
	}
	return false;
}

/* the arm's HAVING that is not `inner aggregate op constant`, evaluated by PostgreSQL on the values of the cell */
static bool
cell_having(AggExec *ex, int k, uint32 g)
{
	SiftAggArm *sa = &ex->spec->arm[k];
	bool		isnull;
	Datum		d;

	for (int j = 0; j < sa->nhp; j++)
	{
		SiftAggAgg *ia = &sa->inner[sa->hsrc[j]];
		double		dv = 0;
		int64		iv = 0;
		bool		notnull = cell_value(&ex->arm[k], ia, sa->hsrc[j], g, &dv, &iv);

		bind_param(ex, j, ia->kind == SAA_SUM_F64 || ia->kind == SAA_AVG_F64 ? Float8GetDatum(dv) : Int64GetDatum(iv), !notnull);
	}
	d = ExecEvalExprSwitchContext(ex->arm[k].hstate, ex->lutcx, &isnull);
	ResetExprContext(ex->lutcx);
	return !isnull && DatumGetBool(d);
}

/* the outer aggregates of group og take the cell (arm k, group g) of the row `row` */
static void
cell_into(AggExec *ex, int k, uint32 g, uint32 og, uint64 row)
{
	SiftAggArm *sa = &ex->spec->arm[k];
	ArmSt	   *a = &ex->arm[k];

	for (int oi = 0; oi < ex->spec->naggs; oi++)
	{
		SiftAggAgg *o = &ex->spec->agg[oi];
		AggAcc	   *acc = &ex->acc[oi];
		bool		pass = true;
		double		dv = 0;
		int64		iv = 0;

		for (int c = 0; pass && c < o->ncond; c++)
		{
			int			src = sa->colsrc[o->cond[c].col];

			pass = cell_value(a, &sa->inner[src], src, g, &dv, &iv) && cond_holds(&o->cond[c], dv, iv);
		}
		if (!pass || o->kind == SAA_GROUPING)
			continue;
		if (o->kind == SAA_O_COUNT_STAR)
		{
			acc->n[og]++;
			continue;
		}
		if (o->kind == SAA_O_COUNT_ROWS)
		{
			if (acc->last[og] != row)
			{
				acc->last[og] = row;
				acc->n[og]++;
			}
			continue;
		}
		if (!cell_value(a, &sa->inner[sa->colsrc[o->col]], sa->colsrc[o->col], g, &dv, &iv))
			continue;
		acc->n[og]++;
		if (o->kind == SAA_O_SUM_I64)
		{
			if (pg_add_s64_overflow(acc->i[og], iv, &acc->i[og]))
				ereport(ERROR,
						(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
						 errmsg("sift: a sum of the aggregate scan exceeds bigint"),
						 errhint("Set sift.enable_agg to off for this query.")));
		}
		else
		{
			acc->d[og] += dv;
			if (!isfinite(dv))
				acc->nf[og] = 1;
		}
	}
}

/*
 * The group that cell g of arm k belongs to (in grouping set `set`): the outer keys the arm (and the set) have, and the set's number.
 * The cell itself is a group of the arm's keys with the hidden ones, which no group keeps.
 */
static uint32
cell_outer_gid(AggExec *ex, int k, uint32 g, int set)
{
	SiftAggSpec *s = ex->spec;
	uint32		mask = s->arm[k].keymask,
				ids[SIFT_AGG_MAX_KEYS] = {0},
				gid = 0;

	if (s->nsets)
	{
		mask &= s->setmask[set];
		ids[s->nkeys - 1] = set + 1;
	}
	for (int j = 0; j < s->nkeys; j++)
		if (mask >> j & 1)
			ids[j] = key_id(ex, j, g);
	if (ex->hash)
		return hash_group(ex, ids);
	for (int j = 0; j < s->nkeys; j++)
		gid += ids[j] * ex->ks[j].mult;
	return gid;
}

/*
 * The cell (arm k, group g) of the row `row` is done: its group exists if it passes the HAVING of the arm, once per grouping set in the
 * sets' groups (the cell stands for the inner group, so a coarser set adds every cell it covers to its group).
 */
static void
flush_cell(AggExec *ex, int k, uint32 g, uint64 row)
{
	SiftAggSpec *s = ex->spec;
	SiftAggArm *sa = &s->arm[k];
	ArmSt	   *a = &ex->arm[k];

	ex->seen[g]--;
	for (int h = 0; h < sa->nhaving; h++)
	{
		double		dv = 0;
		int64		iv = 0;

		if (!cell_value(a, &sa->inner[sa->having[h].col], sa->having[h].col, g, &dv, &iv) || !cond_holds(&sa->having[h], dv, iv))
			return;				/* the cell is no group of the arm */
	}
	if (a->hstate && !cell_having(ex, k, g))
		return;
	if (s->nsets == 0 && sa->hmask == 0)
	{
		ex->seen[g]++;
		cell_into(ex, k, g, g, row);
		return;
	}
	for (int set = 0; set < Max(s->nsets, 1); set++)
		ex->setg[set] = cell_outer_gid(ex, k, g, set);
	if (ex->hash && ex->ngroups > ex->gcap)
		groups_resize(ex, Max(ex->gcap * 2, ex->ngroups), NULL, ex->gcap);
	for (int set = 0; set < Max(s->nsets, 1); set++)
	{
		ex->seen[ex->setg[set]]++;
		cell_into(ex, k, g, ex->setg[set], row);
	}
}

static void
arms_flush(AggExec *ex)
{
	for (uint32 t = 0; t < ex->ntouch; t++)
		flush_cell(ex, ex->touched[t].arm, ex->touched[t].g, ex->cur_order);
	ex->ntouch = 0;
}

/* the tuples of the chunk through every arm: the tuples of a row come together, so a cell lives for one row */
void
arm_chunk(AggExec *ex, Tup *t)
{
	SiftAggSpec *s = ex->spec;
	int			n = t->n;
	uint32		gen;

	ex->n_tuples += n;
	do
	{
		gen = ex->gen;
		for (int k = 0; k < s->narms && gen == ex->gen; k++)
		{
			ArmSt	   *a = &ex->arm[k];

			ex->arm_mask = s->arm[k].keymask | s->arm[k].hmask;
			compute_gids(ex, n);
			memcpy(a->gid, ex->gid, n * sizeof(uint32));
		}
	} while (gen != ex->gen);
	ex->arm_mask = ~(uint32) 0;
	for (int k = 0; k < s->narms; k++)
	{
		SiftAggArm *sa = &s->arm[k];
		ArmSt	   *a = &ex->arm[k];

		memset(a->ok, 1, n);
		for (int f = 0; f < sa->nfilter; f++)
		{
			const SiftAggPred *p = sa->filter[f];
			const uint32 *out = combo_outs(ex, p->leaf);

			for (int j = 0; j < n; j++)
				a->ok[j] &= combo_val(ex, p->leaf, out[j], p->op) != 0;
		}
		for (int ai = 0; ai < sa->ninner; ai++)
			arm_inner_prepare(ex, a, &sa->inner[ai], ai, n);
	}
	for (int j = 0; j < n; j++)
	{
		uint64		row = ex->row_base + ex->ridx[j];

		if (row != ex->cur_order)
		{
			arms_flush(ex);
			ex->cur_order = row;
		}
		for (int k = 0; k < s->narms; k++)
		{
			ArmSt	   *a = &ex->arm[k];
			SiftAggArm *sa = &s->arm[k];
			uint32		g = a->gid[j];

			if (!a->ok[j])
				continue;
			if (a->ord[g] != row)
				cell_start(ex, k, a, g, row);
			for (int ai = 0; ai < sa->ninner; ai++)
				if (a->iok[ai][j])
					cell_add(a, &sa->inner[ai], ai, g, j);
		}
	}
	for (int k = 0; k < s->narms; k++)
		for (int ai = 0; ai < s->arm[k].ninner; ai++)
			if ((s->arm[k].inner[ai].kind == SAA_AVG_F64 || s->arm[k].inner[ai].kind == SAA_X_AVG) &&
				8.0 * ex->n_tuples * ex->arm[k].amax[ai] * ex->arm[k].amax[ai] >= DBL_MAX)
				float_overflow_error();
}

/* ---------- visibility ---------- */

/* ---------- a unit ---------- */

/* the element count of the unit and the offsets of its rows; the owner of every element is made when something reads it (unit_parent) */
static void
build_parents(AggExec *ex)
{
	ex->offs = elem_offs(ex, ex->spec->group);
	ex->nelem = ex->offs[ex->unit.nrow];
	ex->parent = NULL;
}

/* the row that owns each element of the array or object leaf `group` (the unnested one is unit_parent's) */
static const uint32 *
group_parent(AggExec *ex, int group)
{
	const uint32 *offs;
	uint32	   *par;

	if (group == ex->spec->group)
	{
		build_parents(ex);
		return unit_parent(ex);
	}
	offs = elem_offs(ex, group);
	par = MemoryContextAlloc(ex->ucxt, (Size) offs[ex->unit.nrow] * sizeof(uint32) + SIFT_AGG_PAD);
	sift_colk->expand_parents(offs + 1, ex->unit.nrow, par);
	return par;
}

/*
 * The conjuncts that evaluate an expression PostgreSQL may fail on (SiftAggPred.late) are applied to the items that passed every other
 * conjunct and the snapshot check, in the order of the query, each to the survivors of the ones before it: the normal plan never
 * evaluates an expression for a row that an earlier condition (a guard against a zero divisor) or its snapshot leaves out.
 */
static void
apply_late(AggExec *ex, uint64 *mask, int total)
{
	for (int i = 0; i < ex->spec->npred; i++)
	{
		SiftAggPred *p = ex->spec->pred[i];

		if (!p->late)
			continue;
		for (int st = 0; st < total; st += VEC)
		{
			int			n = Min(VEC, total - st);
			uint64		t[VECW + PADW];

			ex->alive = mask + st / 64;
			eval_pred(ex, p, ex->qlevel, st, n, t);
			ex->alive = NULL;
			sift_colk->bits_and(mask + st / 64, t, (n + 63) / 64);
		}
	}
}

/* ---------- pre-aggregates (docs/COL-PREAGG.md) ---------- */

/* the leaf an aggregate reads: its column, the only leaf of its numeric program; -1: count(*) */
static int
pa_leaf(const SiftAggAgg *a)
{
	return a->kind == SAA_COUNT_STAR ? -1 : a->num.n ? a->num.step[0].leaf : a->leaf;
}

/* v into the state of aggregate ai in group g, as the rows it stands for would have been added */
static void
preagg_add(AggExec *ex, int ai, uint32 g, const PaVal *v)
{
	const SiftAggAgg *a = &ex->spec->agg[ai];
	AggAcc	   *acc = &ex->acc[ai];

	switch (a->kind)
	{
		case SAA_COUNT_STAR:
			acc->n[g] += v->rows;
			break;
		case SAA_COUNT:
			acc->n[g] += v->nn;
			break;
		case SAA_SUM_INT:
		case SAA_AVG_INT:
			if (v->sum != (int64) v->sum || pg_add_s64_overflow(acc->i[g], (int64) v->sum, &acc->i[g]))
				ereport(ERROR, (errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE), errmsg("bigint out of range")));
			acc->n[g] += v->nn;
			break;
		case SAA_SUM_NUM:
		case SAA_AVG_NUM:
			acc->w[g] += v->sum;
			acc->n[g] += v->nn;
			break;
		case SAA_SUM_F64:
		case SAA_AVG_F64:
			acc->d[g] += v->sumf;
			acc->n[g] += v->nn;
			break;
		default:					/* SAA_MIN, SAA_MAX */
			if (v->has_mm)
			{
				bool		ismin = a->kind == SAA_MIN;
				int64		x = ismin ? v->min : v->max;
				sift_i128	cur = a->num.n ? acc->w[g] : acc->i[g];

				if (ex->lay->leaf[pa_leaf(a)].vtype == SCV_F64)
				{
					double		f = ismin ? v->minf : v->maxf;

					if (acc->n[g] == 0 || (ismin ? float8_cmp_internal(f, acc->d[g]) < 0 : float8_cmp_internal(f, acc->d[g]) > 0))
						acc->d[g] = f;
				}
				else if (acc->n[g] == 0 || (ismin ? x < cur : x > cur))
				{
					if (a->num.n)
						acc->w[g] = x;
					else
						acc->i[g] = x;
				}
				acc->n[g] += v->nn;
			}
	}
}

/* group g takes rows rows, the aggregates their values v[] */
void
preagg_fold(AggExec *ex, uint32 g, int64 rows, const PaVal *v)
{
	ex->seen[g] += rows;
	for (int ai = 0; ai < ex->spec->naggs; ai++)
		preagg_add(ex, ai, g, &v[ai]);
}

/* the unit's zone of every leaf the conjuncts compare lies inside the comparison, and holds no NULL: they hold for all the rows */
static bool
preagg_zones(AggExec *ex)
{
	for (int k = 0; k < ex->nprune; k++)
	{
		const SiftSrcPrune *z = &ex->prune[k];
		SiftSrcUnitAgg a;

		if (z->leaf != ex->pa.dim &&
			(!sift_colsrc_unit_agg(ex->src, &ex->unit, z->leaf, &a) || a.nnull > 0 || !a.has_minmax || a.min < z->lo || a.max > z->hi))
			return false;
	}
	return true;
}

/* no key: the unit's rows are one group, from the sums of the leaves */
static bool
preagg_sums(AggExec *ex)
{
	PaVal	   *v = MemoryContextAllocZero(ex->ucxt, Max(ex->spec->naggs, 1) * sizeof(PaVal));
	int			nrow = ex->unit.nrow;

	for (int ai = 0; ai < ex->spec->naggs; ai++)
	{
		const SiftAggAgg *a = &ex->spec->agg[ai];
		int			leaf = pa_leaf(a);
		SiftSrcUnitAgg ua;

		v[ai].rows = v[ai].nn = nrow;
		if (leaf < 0)
			continue;
		if (!sift_colsrc_unit_agg(ex->src, &ex->unit, leaf, &ua) || (a->kind != SAA_COUNT && a->kind != SAA_MIN && a->kind != SAA_MAX && !ua.has_sums))
			return false;
		v[ai] = (PaVal) {nrow, ua.nvals - ua.nnull, ua.sum, ua.has_minmax, ua.min, ua.max};
	}
	preagg_fold(ex, 0, nrow, v);
	return true;
}

/*
 * The groups of the histogram that pass the conjuncts on the key's column (a bitmap; NULL: all): the tests run on the groups as
 * on the rows of a unit that has one per group, the column's vector being the histogram's keys.
 */
static uint64 *
preagg_keep(AggExec *ex, const SiftSrcHist *h)
{
	SiftAggSpec *s = ex->spec;
	int			ng = h->ngroups,
				nw = (ng + 63) / 64;
	uint64	   *keep = NULL;
	uint32	   *data = NULL;

	for (int i = 0; i < s->npred; i++)
	{
		if (sift_aggspec_pred_leaf(s->pred[i], ex->lay, ex->nbase, s->group) != ex->pa.dim)
			continue;
		if (keep == NULL)
		{
			uint8	   *isnull = h->has_null ? MemoryContextAllocZero(ex->ucxt, ng + SIFT_AGG_PAD) : NULL;

			keep = MemoryContextAlloc(ex->ucxt, (nw + PADW) * sizeof(uint64));
			fill_ones(keep, ng);
			data = (uint32 *) TYPEALIGN(SIFT_SIMD_ALIGN, MemoryContextAllocZero(ex->ucxt, ng * sizeof(uint32) + SIFT_AGG_PAD + SIFT_SIMD_ALIGN));
			for (int k = 0; k < ng - h->has_null; k++)
				data[k] = (uint32) h->key[k];
			if (h->has_null)
			{
				isnull[ng - 1] = 1;
				data[ng - 1] = h->dict ? h->dict->n : 0;
			}
			ex->uvec[ex->pa.dim] = (SiftSrcVec) {data, isnull, h->dict, ng, NULL};
			ex->uvec_ok[ex->pa.dim] = true;
		}
		for (int st = 0; st < ng; st += VEC)
		{
			int			n = Min(VEC, ng - st);
			uint64		t[VECW + PADW];

			eval_pred(ex, s->pred[i], s->group >= 0, st, n, t);
			sift_colk->bits_and(keep + st / 64, t, (n + 63) / 64);
		}
	}
	ex->uvec_ok[ex->pa.dim] = false;
	return keep;
}

/*
 * The ids of key ki for the kept groups or tuples: the unit's dictionary entries, or the integers. A group is NULL when isnull[g] (NULL: the
 * groups from nonnull on are, the NULL group of a histogram being its last)
 */
void
preagg_key_ids(AggExec *ex, int ki, const int64 *key, const uint8 *isnull, uint32 nonnull, const SiftSrcDict *dict, const uint32 *kept, int nkept,
			   uint32 *ids)
{
	KeyState   *k = &ex->ks[ki];
	int32		mn = PG_INT32_MAX,
				mx = PG_INT32_MIN;
	const uint32 *map = NULL;
	bool		cover = false;

#define PA_NULL(g) (isnull ? isnull[g] : (g) >= nonnull)
	if (dict)
		map = dict_kmap(ex, dict_slot(ex, dict), dict, ki);
	for (int j = 0; j < nkept && !dict; j++)
		if (!PA_NULL(kept[j]))
		{
			mn = Min(mn, (int32) key[kept[j]]);
			mx = Max(mx, (int32) key[kept[j]]);
		}
	if (!dict)
		cover = key_cover(ex, ki, mn <= mx, mn, mx);
	for (int j = 0; j < nkept; j++)
	{
		uint32		g = kept[j];

		if (dict)
			ids[j] = map[PA_NULL(g) ? dict->n : (uint32) key[g]];
		else if (PA_NULL(g))
			ids[j] = 0;
		else
			ids[j] = cover ? ((const uint32 *) k->rt.tab)[(int32) key[g] - k->rt.lo] : ident_wide_id(ex, k, (int32) key[g]);
	}
#undef PA_NULL
}

/* the groups of the table for n tuples that have the key ids ids[key * n + i]: compute_gids for ids that are known */
void
ids_to_gids(AggExec *ex, int n, const uint32 *ids, uint32 *gid)
{
	int			nk = ex->spec->nkeys;

	if (nk == 0)
	{
		memset(gid, 0, n * sizeof(uint32));
		return;
	}
	for (int j = 0; j < nk && !ex->hash; j++)
		if (key_used(&ex->ks[j]) > ex->ks[j].cap)
		{
			direct_grow(ex);
			break;
		}
	if (!ex->hash)
	{
		for (int i = 0; i < n; i++)
		{
			uint32		g = 0;

			for (int j = 0; j < nk; j++)
				g += ids[(Size) j * n + i] * ex->ks[j].mult;
			gid[i] = g;
		}
		return;
	}
	for (int i = 0; i < n; i++)
	{
		uint32		k[SIFT_AGG_MAX_KEYS] = {0};

		for (int j = 0; j < nk; j++)
			k[j] = ids[(Size) j * n + i];
		gid[i] = hash_group(ex, k);
	}
	if (ex->ngroups > ex->gcap)
		groups_resize(ex, Max(ex->gcap * 2, ex->ngroups), NULL, ex->gcap);
}

/* the values of a float8 leaf of the unit are finite and so small that the sum of their squares, which avg(float8) is owed to check, cannot overflow */
#define PA_FLOAT_MAX	1e100

static bool
preagg_floats(AggExec *ex, int leaf, bool bounded)
{
	SiftSrcUnitAgg a;

	return sift_colsrc_unit_agg(ex->src, &ex->unit, leaf, &a) && !a.has_nan &&
		(!bounded || a.nnull == a.nvals || (a.has_minmax && fabs(a.min_f) <= PA_FLOAT_MAX && fabs(a.max_f) <= PA_FLOAT_MAX));
}

/* the groups of the unit's histogram of the key's column, or of the column the conjuncts test (no key: one group) */
static bool
preagg_hist(AggExec *ex)
{
	SiftAggSpec *s = ex->spec;
	SiftSrcHist h;
	int		   *slot = MemoryContextAlloc(ex->ucxt, Max(s->naggs, 1) * sizeof(int));
	PaVal	   *v = MemoryContextAllocZero(ex->ucxt, Max(s->naggs, 1) * sizeof(PaVal));
	uint32	   *kept,
			   *ids,
			   *gid;
	uint64	   *keep;
	int			nkept = 0;

	if (!sift_colsrc_unit_hist(ex->src, &ex->unit, ex->pa.dim, &h))
		return false;
	for (int ai = 0; ai < s->naggs; ai++)
	{
		int			leaf = pa_leaf(&s->agg[ai]);
		int			m = 0;

		while (leaf >= 0 && leaf != ex->pa.dim && m < h.nmeas && h.meas[m] != leaf)
			m++;
		if (leaf >= 0 && leaf != ex->pa.dim && (m == h.nmeas || (s->agg[ai].kind != SAA_COUNT && h.sum[m] == NULL && h.sumf[m] == NULL)))
			return false;
		if (leaf >= 0 && (s->agg[ai].kind == SAA_SUM_F64 || s->agg[ai].kind == SAA_AVG_F64) && !preagg_floats(ex, leaf, true))
			return false;
		slot[ai] = leaf < 0 ? -1 : leaf == ex->pa.dim ? -2 : m;
	}
	keep = preagg_keep(ex, &h);
	kept = MemoryContextAlloc(ex->ucxt, Max(h.ngroups, 1) * sizeof(uint32));
	for (uint32 k = 0; k < h.ngroups; k++)
		if (keep == NULL || sift_colsrc_bit(keep, k))
			kept[nkept++] = k;
	ids = MemoryContextAllocZero(ex->ucxt, Max(nkept, 1) * sizeof(uint32));
	gid = MemoryContextAllocZero(ex->ucxt, Max(nkept, 1) * sizeof(uint32));
	if (s->nkeys == 1)
		preagg_key_ids(ex, 0, h.key, NULL, h.ngroups - h.has_null, h.dict, kept, nkept, ids);
	ids_to_gids(ex, nkept, ids, gid);
	for (int j = 0; j < nkept; j++)
	{
		for (int ai = 0; ai < s->naggs; ai++)
		{
			int			m = slot[ai];

			v[ai].rows = v[ai].nn = h.count[kept[j]];
			if (m == -2)
				v[ai].nn = kept[j] < h.ngroups - h.has_null ? h.count[kept[j]] : 0;
			else if (m >= 0)
			{
				v[ai].nn = h.nn[m][kept[j]];
				v[ai].sum = h.sum[m] ? h.sum[m][kept[j]] : 0;
				v[ai].sumf = h.sumf[m] ? h.sumf[m][kept[j]] : 0;
			}
		}
		preagg_fold(ex, gid[j], h.count[kept[j]], v);
	}
	return true;
}

/* the position of leaf among the dims of the rollup, -1 if it holds none */
static int
rollup_dim(const SiftSrcRollup *r, int leaf)
{
	for (int j = 0; j < r->ndims; j++)
		if (r->dim[j] == leaf)
			return j;
	return -1;
}

/* the tuples of the rollup that pass the element conjuncts (a bitmap; NULL: all): they run on the tuples as on the elements, each dim's vector being the tuples' keys */
static uint64 *
rollup_keep(AggExec *ex, const SiftSrcRollup *r)
{
	SiftAggSpec *s = ex->spec;
	int			nt = r->ntuples,
				nw = (nt + 63) / 64;
	uint64	   *keep = NULL;

	for (int i = 0; i < s->npred; i++)
	{
		if (s->pred[i]->level != 1)
			continue;
		if (keep == NULL)
		{
			keep = MemoryContextAlloc(ex->ucxt, (nw + PADW) * sizeof(uint64));
			fill_ones(keep, nt);
			for (int d = 0; d < ex->pa.ndims; d++)
			{
				int			j = rollup_dim(r, ex->pa.rdim[d]);
				uint32	   *data = (uint32 *) TYPEALIGN(SIFT_SIMD_ALIGN, MemoryContextAllocZero(ex->ucxt, nt * sizeof(uint32) + SIFT_AGG_PAD + SIFT_SIMD_ALIGN));

				for (int t = 0; t < nt; t++)
					data[t] = r->isnull[j] && r->isnull[j][t] ? (r->dict[j] ? r->dict[j]->n : 0) : (uint32) r->key[j][t];
				ex->uvec[ex->pa.rdim[d]] = (SiftSrcVec) {data, r->isnull[j], r->dict[j], nt, NULL};
				ex->uvec_ok[ex->pa.rdim[d]] = true;
			}
		}
		for (int st = 0; st < nt; st += VEC)
		{
			int			n = Min(VEC, nt - st);
			uint64		t[VECW + PADW];

			eval_pred(ex, s->pred[i], 1, st, n, t);
			sift_colk->bits_and(keep + st / 64, t, (n + 63) / 64);
		}
	}
	for (int d = 0; keep && d < ex->pa.ndims; d++)
		ex->uvec_ok[ex->pa.rdim[d]] = false;
	return keep;
}

/* The unit's rollup of the list: its tuples that pass the conjuncts, folded into the groups of the keys. false: the unit lacks a dim or a measure the query reads */
static bool
preagg_rollup(AggExec *ex)
{
	SiftAggSpec *s = ex->spec;
	SiftSrcRollup r;
	int		   *slot = MemoryContextAlloc(ex->ucxt, Max(s->naggs, 1) * sizeof(int));
	PaVal	   *v = MemoryContextAllocZero(ex->ucxt, Max(s->naggs, 1) * sizeof(PaVal));
	uint32	   *kept,
			   *ids,
			   *gid;
	uint64	   *keep;
	int			nkept = 0;

	ex->pa_miss = true;
	if (!sift_colsrc_unit_rollup(ex->src, &ex->unit, ex->pa.list, &r))
		return false;
	for (int d = 0; d < ex->pa.ndims; d++)
		if (rollup_dim(&r, ex->pa.rdim[d]) < 0)
			return false;
	for (int ai = 0; ai < s->naggs; ai++)
	{
		const SiftAggAgg *a = &s->agg[ai];
		int			leaf = pa_leaf(a),
					m = 0;
		bool		mm = a->kind == SAA_MIN || a->kind == SAA_MAX;

		if (leaf < 0)
		{
			slot[ai] = -1;
			continue;
		}
		if (a->kind == SAA_COUNT && rollup_dim(&r, leaf) >= 0)
		{
			slot[ai] = -2 - rollup_dim(&r, leaf);
			continue;
		}
		while (m < r.nmeas && r.meas[m] != leaf)
			m++;
		if (m == r.nmeas)
			return false;
		if (ex->lay->leaf[leaf].vtype == SCV_F64 && a->kind != SAA_COUNT && !preagg_floats(ex, leaf, !mm))
		{
			ex->pa_miss = false;
			return false;
		}
		slot[ai] = m;
	}
	keep = rollup_keep(ex, &r);
	kept = MemoryContextAlloc(ex->ucxt, Max(r.ntuples, 1) * sizeof(uint32));
	for (uint32 t = 0; t < r.ntuples; t++)
		if (keep == NULL || sift_colsrc_bit(keep, t))
			kept[nkept++] = t;
	ids = MemoryContextAllocZero(ex->ucxt, Max((Size) nkept * s->nkeys, 1) * sizeof(uint32));
	gid = MemoryContextAllocZero(ex->ucxt, Max(nkept, 1) * sizeof(uint32));
	for (int k = 0; k < s->nkeys; k++)
	{
		int			j = rollup_dim(&r, s->key[k].leaf);

		preagg_key_ids(ex, k, r.key[j], r.isnull[j], UINT32_MAX, r.dict[j], kept, nkept, ids + (Size) k * nkept);
	}
	ids_to_gids(ex, nkept, ids, gid);
	for (int i = 0; i < nkept; i++)
	{
		uint32		t = kept[i];

		for (int ai = 0; ai < s->naggs; ai++)
		{
			int			m = slot[ai];

			v[ai] = (PaVal) {.rows = r.count[t], .nn = r.count[t]};
			if (m <= -2)
				v[ai].nn = r.isnull[-2 - m] && r.isnull[-2 - m][t] ? 0 : r.count[t];
			else if (m >= 0)
			{
				v[ai].nn = r.nn[m][t];
				v[ai].has_mm = r.nn[m][t] > 0;
				if (r.sum[m])
					v[ai].sum = r.sum[m][t];
				if (r.sumf[m])
					v[ai].sumf = r.sumf[m][t];
				if (r.min[m])
				{
					v[ai].min = r.min[m][t];
					v[ai].max = r.max[m][t];
				}
				if (r.minf[m])
				{
					v[ai].minf = r.minf[m][t];
					v[ai].maxf = r.maxf[m][t];
				}
			}
		}
		preagg_fold(ex, gid[i], r.count[t], v);
	}
	return true;
}

static bool
preagg_unit(AggExec *ex)
{
	if (!sift_colsrc_unit_exact(ex->src, &ex->unit) || !preagg_zones(ex))
		return false;
	if (ex->pa.list < 0)
		return ex->pa.dim < 0 ? preagg_sums(ex) : preagg_hist(ex);
	if (preagg_rollup(ex))
		return true;
	/* the unit is clean but its rollup lacks something: scan it now, and ask for the rollup it should have */
	if (ex->pa_miss)
	{
		ex->n_rollup_miss++;
		ex->n_rollup_queued += sift_colsrc_unit_rollup_miss(ex->src, &ex->unit);
	}
	return false;
}

static void
preagg_init(AggExec *ex)
{
	SiftSrcPreagg pa;

	ex->pa_on = false;
	if (!sift_agg_preagg)
		return;
	sift_colsrc_preagg(ex->index ? ex->index : ex->heap, &pa);
	ex->pa_on = sift_aggspec_preagg(ex->spec, ex->lay, ex->nbase, &pa, &ex->pa);
	/* a comparison with NULL keeps no row, and leaves no interval for the zone to lie inside; nor does an IN list with gaps (or a list that prune_init gave no zone) */
	foreach_ptr(SiftAggPred, p, sift_aggspec_zone_cmps(ex->spec, ex->lay, ex->nbase))
	{
		PredState  *st = &ex->ps[p->id];

		if (!st->ready)
			eval_operand(ex, p, st);
		ex->pa_on &= !st->cnull && (p->kind != SAP_IN || st->in_dense);
	}
}
/* A unit whose zone of the first sort leaf has nothing that sorts before the n-th best so far is not read */
static bool
topn_viable(AggExec *ex)
{
	SiftSrcZone z;

	if (!sift_topn_wants_zones(ex->topn))
		return true;
	sift_colsrc_zone(ex->src, &ex->unit, sift_topn_leaf(ex->topn, 0), &z);
	if (sift_topn_zone_viable(ex->topn, &z))
		return true;
	sift_topn_skip(ex->topn);
	return false;
}

/* the rows that pass go to the heap of the best n, by the sort leaves (agg/topn.h) */
static void
topn_unit(AggExec *ex, const uint64 *rmask, int nrow)
{
	sift_topn_unit(ex->topn, ex->src, &ex->unit, get_vec(ex, sift_topn_leaf(ex->topn, 0)), rmask, nrow);
}

/* *rows gets the rows of the unit that hold a value the index cannot store in leaf (a numeric NaN aside: the readers evaluate it). An element of a child leaf counts for its row. */
static void
leaf_overflow_rows(AggExec *ex, int leaf, uint64 **rows)
{
	const SiftSrcLeaf *l = &ex->lay->leaf[leaf];
	const SiftSrcVec *v;
	const uint32 *par;

	if (l->typid == NUMERICOID || !sift_colsrc_overflow(ex->src, &ex->unit, leaf) || !(v = get_vec(ex, leaf))->ovf)
		return;
	if (!*rows)
		*rows = MemoryContextAllocZero(ex->ucxt, ((ex->unit.nrow + 63) / 64 + PADW) * sizeof(uint64));
	par = l->parent >= 0 ? group_parent(ex, l->parent) : NULL;
	for (int64 e = 0; e < v->n; e++)
		if (sift_colsrc_bit(v->ovf, e))
		{
			uint32		r = par ? par[e] : e;

			(*rows)[r >> 6] |= UINT64CONST(1) << (r & 63);
		}
}

/*
 * The rows of the unit that hold a value the index cannot store in a leaf the scan reads, NULL when there is none. The leaves after the source's
 * read their values from one of its leaves (the value column of a member of an object, the array of a length): theirs count too.
 */
static uint64 *
overflow_rows(AggExec *ex, bool rechecked)
{
	SiftAggSpec *s = ex->spec;
	uint64	   *rows = NULL;

	for (int i = 0; i < s->nleaves; i++)
		if (s->leaves[i] < ex->nbase && (s->recheck[i] != SRK_NONE) == rechecked)
			leaf_overflow_rows(ex, s->leaves[i], &rows);
	for (int j = 0; j < s->nvleaf && !rechecked; j++)
		if (!s->vleaf[j].types)
			leaf_overflow_rows(ex, s->vleaf[j].count ? s->vleaf[j].obj : ex->vl[j].val, &rows);
	return rows;
}

/*
 * A unit may hold values the index cannot store (the writer's committed after the plan was made, in flight or rolled back, as the plan could
 * not know); the rows of another snapshot or none are not the scan's, a row it sees is: the normal plan reads it from the heap.
 * rechecked: the leaves the source reads again (a row of the stored unit that has such a value is not in its live rows: the unit that follows
 * has it, its values as SQL gives them) and what is left is the value SQL raises for. The source has the error of the row; it is raised when the
 * row reaches the expression, after the conjuncts of the rows, as SQL raises it.
 */
static void
check_overflow(AggExec *ex, const uint64 *live, bool rechecked)
{
	int			nrw = (ex->unit.nrow + 63) / 64;
	uint64	   *rows = overflow_rows(ex, rechecked);

	if (!rows)
		return;
	sift_colk->bits_and(rows, live, nrw);
	sift_colsrc_vet(ex->src, &ex->unit, rows);
	if (sift_colk->bits_count(rows, nrw) == 0)
		return;
	if (rechecked)
		sift_colsrc_overflow_raise(ex->src, &ex->unit, rows);
	overflow_error();
}

/* the leaves are read for the rows that still count when they are fewer than a SPARSE_RATIOth of the unit */
#define SPARSE_RATIO	8

/* the rows whose value has one of the JSON types of each SAP_TYPE conjunct; false when none is left */
static bool
type_filters(AggExec *ex, uint64 *rmask)
{
	for (int i = 0; i < ex->spec->npred; i++)
	{
		SiftAggPred *p = ex->spec->pred[i];

		if (p->kind == SAP_TYPE && !sift_colsrc_filter_types(ex->src, &ex->unit, p->leaf, p->op, rmask))
			return false;
	}
	return true;
}

/* the rows that pass each SAP_WIDE conjunct, in the order wide_init gave them to the source; false when none is left */
static bool
wide_filters(AggExec *ex, uint64 *rmask)
{
	int			k = 0;

	for (int i = 0; i < ex->spec->npred; i++)
		if (ex->spec->pred[i]->kind == SAP_WIDE && !sift_colsrc_filter_wide(ex->src, &ex->unit, k++, rmask))
			return false;
	return true;
}

static void
process_unit(AggExec *ex)
{
	SiftSrcUnit *u = &ex->unit;
	SiftAggSpec *s = ex->spec;
	int			nrow = u->nrow,
				nrw = (nrow + 63) / 64;
	uint64	   *rmask;
	bool		elem = s->group >= 0;

	MemoryContextReset(ex->ucxt);
	memset(ex->uvec_ok, 0, ex->lay->nleaves * sizeof(bool));
	memset(ex->gofs, 0, ex->lay->nleaves * sizeof(uint32 *));
	memset(ex->gpar, 0, ex->lay->nleaves * sizeof(uint32 *));
	for (int i = 0; i < s->npreds_total; i++)
		ex->ps[i].rowmask_ok = false;
	ex->n_units++;
	if (nrow > 0 && ex->pa_on && preagg_unit(ex))
	{
		ex->n_pre_units++;
		ex->row_base += nrow;
		CHECK_FOR_INTERRUPTS();
		return;
	}
	ex->n_rows += nrow;
	if (nrow == 0)
		return;
	if (sift_aggbe_run_unit(ex->be, ex->be_state, &ex->unit))
	{
		ex->row_base += nrow;
		CHECK_FOR_INTERRUPTS();
		return;
	}
	if (ex->topn && !topn_viable(ex))
		return;
	rmask = MemoryContextAlloc(ex->ucxt, (nrw + PADW) * sizeof(uint64));
	ex->rsel = NULL;
	sift_colsrc_live(ex->src, u, rmask);
	check_overflow(ex, rmask, false);
	if ((ex->ntype > 0 && !type_filters(ex, rmask)) || (ex->nwide > 0 && !wide_filters(ex, rmask)) ||
		(ex->nfilter > 0 && !sift_colsrc_filter(ex->src, u, rmask)))
	{
		ex->row_base += nrow;
		return;
	}
	/* a few rows left: the leaves are decoded for them only */
	if ((ex->nfilter > 0 || ex->ntype > 0 || ex->nwide > 0) && sift_colsrc_can_read_sel(ex->src) && sift_colk->bits_count(rmask, nrw) * SPARSE_RATIO < nrow)
		ex->rsel = rmask;
	/* row level conjuncts */
	for (int i = 0; i < s->npred; i++)
	{
		SiftSrcStrTest *str = ex->ps[s->pred[i]->id].str;

		if (s->pred[i]->level == 0 && !s->pred[i]->late && !(ex->pushed && ex->pushed[i]) && !(str && sift_colsrc_filter_str(ex->src, u, str, rmask)))
			sift_colk->bits_and(rmask, row_bitmap(ex, s->pred[i]), nrw);
	}
	ex->qlevel = elem;
	if (ex->recheck)
		check_overflow(ex, rmask, true);
	if (!elem)
	{
		sift_colsrc_vet(ex->src, u, rmask);
		apply_late(ex, rmask, nrow);
		if (ex->topn)
			topn_unit(ex, rmask, nrow);
		else
			for (int st = 0; st < nrow; st += VEC)
				agg_chunk(ex, st, Min(VEC, nrow - st), rmask + st / 64);
	}
	else
	{
		uint64	   *em;
		int			m;
		uint64		cm[VECW + PADW];
		bool		allrows;

		build_parents(ex);
		m = ex->nelem;
		/* no row is out: so is no element, and the owners of the elements are not needed to tell */
		allrows = sift_colk->bits_count(rmask, nrw) == nrow;
		em = MemoryContextAlloc(ex->ucxt, ((m + 63) / 64 + PADW) * sizeof(uint64));
		for (int st = 0; st < m; st += VEC)
		{
			int			n = Min(VEC, m - st),
						nw = (n + 63) / 64;

			if (allrows)
				fill_ones(cm, n);
			else
				sift_aggk->gather_bits(cm, rmask, unit_parent(ex) + st, n);
			for (int i = 0; i < s->npred; i++)
				if (s->pred[i]->level == 1 && !s->pred[i]->late)
				{
					uint64		t[VECW + PADW];

					eval_pred(ex, s->pred[i], 1, st, n, t);
					sift_colk->bits_and(cm, t, nw);
				}
			memcpy(em + st / 64, cm, nw * sizeof(uint64));
		}
		/* the rows that keep an element, checked against the snapshot: nothing to check in a unit whose pages are all visible */
		if (sift_colsrc_vet_needed(ex->src, u))
		{
			uint64	   *rv = MemoryContextAllocZero(ex->ucxt, (nrw + PADW) * sizeof(uint64)),
					   *seen = MemoryContextAlloc(ex->ucxt, (nrw + PADW) * sizeof(uint64));

			rows_of_elems(ex->offs, nrow, em, rv);
			memcpy(seen, rv, nrw * sizeof(uint64));
			sift_colsrc_vet(ex->src, u, rv);
			/* the elements of a row the snapshot does not see go */
			for (int w = 0; w < nrw; w++)
				for (uint64 b = seen[w] & ~rv[w]; b; b &= b - 1)
				{
					int			r = w * 64 + pg_rightmost_one_pos64(b);

					clear_bit_range(em, ex->offs[r], ex->offs[r + 1]);
				}
		}
		apply_late(ex, em, m);
		for (int st = 0; st < m; st += VEC)
			agg_chunk(ex, st, Min(VEC, m - st), em + st / 64);
	}
	ex->row_base += nrow;
	CHECK_FOR_INTERRUPTS();
}

/* ---------- the scan ---------- */

static void
init_states(AggExec *ex)
{
	SiftAggSpec *s = ex->spec;

	ex->ps = MemoryContextAllocZero(ex->cxt, Max(s->npreds_total, 1) * sizeof(PredState));
	ex->ks = MemoryContextAllocZero(ex->cxt, Max(s->nkeys, 1) * sizeof(KeyState));
}

static void
register_pred(AggExec *ex, SiftAggPred *p)
{
	PredState  *st;

	if (p == NULL)
		return;
	st = &ex->ps[p->id];
	if (p->kind == SAP_LUT)
	{
		MemoryContext old = MemoryContextSwitchTo(ex->cxt);

		st->expr = init_lut_expr(ex, p->expr);
		st->param = find_param(p->expr);
		rt_init(&st->rt, 1);
		if (ex->lay->leaf[p->leaf].vtype == SCV_I32)
			st->null_result = eval_bool(ex, st->expr, (Datum) 0, true);
		MemoryContextSwitchTo(old);
	}
	for (int i = 0; i < p->nchild; i++)
		register_pred(ex, p->child[i]);
}

/* every predicate the scan evaluates: the conjuncts of the rows, the FILTER of the aggregates and of the inner aggregates of the arms */
static void
register_preds(AggExec *ex)
{
	SiftAggSpec *s = ex->spec;

	for (int i = 0; i < s->npred; i++)
		register_pred(ex, s->pred[i]);
	for (int i = 0; i < s->naggs; i++)
		register_pred(ex, s->agg[i].filter);
	for (int k = 0; k < s->narms; k++)
		for (int i = 0; i < s->arm[k].ninner; i++)
			register_pred(ex, s->arm[k].inner[i].filter);
}

/* The interval each leaf's comparisons leave: a unit whose zone misses it holds no row that passes (docs/COL-PLAN.md, "Zone pruning") */
static void
prune_init(AggExec *ex)
{
	List	   *cmps = sift_aggspec_zone_cmps(ex->spec, ex->lay, ex->nbase);

	ex->prune = MemoryContextAlloc(ex->cxt, Max(list_length(cmps), 1) * sizeof(SiftSrcPrune));
	foreach_ptr(SiftAggPred, p, cmps)
	{
		PredState  *st = &ex->ps[p->id];
		SiftSrcPrune *z = NULL;
		int64		lo = PG_INT64_MIN,
					hi = PG_INT64_MAX;

		if (!st->ready)
			eval_operand(ex, p, st);
		if (st->cnull)
			continue;
		if (p->kind == SAP_IN)
		{
			if (st->in_sorted || st->in_nlut == 0)
				continue;
			lo = st->in_lo;
			hi = (int64) st->in_lo + st->in_nlut - 1;
		}
		else
		{
			bool		num = ex->lay->leaf[p->leaf].typid == NUMERICOID;
			SiftCmpOp	op = num ? (SiftCmpOp) st->nop : (SiftCmpOp) p->op;
			int64		c = num ? (int64) st->cn : st->ci;

			if (num && st->konst >= 0)
			{
				/* one result for every number (a constant no scaled integer equals, or beyond all of them): the whole range passes, or no value does */
				if (st->konst == 0)
				{
					lo = PG_INT64_MAX;
					hi = PG_INT64_MIN;
				}
			}
			else
			{
				if (op != SCMP_GT && op != SCMP_GE)
					hi = op == SCMP_LT && c > PG_INT64_MIN ? c - 1 : c;
				if (op != SCMP_LT && op != SCMP_LE)
					lo = op == SCMP_GT && c < PG_INT64_MAX ? c + 1 : c;
			}
		}
		for (int k = 0; k < ex->nprune; k++)
			if (ex->prune[k].leaf == p->leaf)
				z = &ex->prune[k];
		if (z == NULL)
		{
			z = &ex->prune[ex->nprune++];
			*z = (SiftSrcPrune) {p->leaf, PG_INT64_MIN, PG_INT64_MAX};
		}
		z->hi = Min(z->hi, hi);
		z->lo = Max(z->lo, lo);
	}
}

/* The text tests of the query a source can run on the encoded form: they prune its units and narrow the rows of each (process_unit) */
static void
str_init(AggExec *ex)
{
	SiftSrcStrTest *all = palloc(Max(ex->spec->npred, 1) * sizeof(SiftSrcStrTest));
	int			n = 0;

	for (int i = 0; i < ex->spec->npred; i++)
		if (sift_aggspec_str_test(ex->spec->pred[i], ex->lay, ex->nbase, &all[n]))
			ex->ps[ex->spec->pred[i]->id].str = &all[n++];
	if (n > 0)
		sift_colsrc_prune_str(ex->src, all, n);
}

/*
 * The conjuncts the source can apply on its stored integer values, as filters: a comparison of an integer-valued leaf with a constant (a
 * numeric as its scaled integer) and a short IN list. The text tests have their own path (str_init).
 */
#define FILTER_MAX_SET	8

/* a comparison or short IN list of an integer-valued leaf */
static bool
int_filter(AggExec *ex, SiftAggPred *p, SiftSrcFilter *f)
{
	PredState  *st = &ex->ps[p->id];
	const SiftSrcLeaf *lf = &ex->lay->leaf[p->leaf];

	if (lf->vtype != SCV_I32 && lf->vtype != SCV_I64)
		return false;
	if (!st->ready)
		eval_operand(ex, p, st);
	if (st->cnull)
		return false;
	f->leaf = p->leaf;
	if (p->kind == SAP_CMP)
	{
		bool		num = lf->typid == NUMERICOID;

		if (num && st->konst >= 0)
		{
			/* every value passes (a NaN too: not null) or none does; the NaN rows of the others are the executor's */
			if (st->konst == 0 || !st->nan_res)
				return false;
			f->op = SSF_NOTNULL;
		}
		else
		{
			f->op = (SiftSrcFilterOp) (num ? st->nop : p->op);
			f->val = num ? (int64) st->cn : st->ci;
		}
		return true;
	}
	{
		int64	   *set = palloc(FILTER_MAX_SET * sizeof(int64));
		int			n = 0;

		if ((p->op && st->in_hasnull) || st->in_sorted)
			return false;
		for (uint32 i = 0; i < st->in_nlut; i++)
			if (st->in_lut[i] && n++ < FILTER_MAX_SET)
				set[n - 1] = (int64) st->in_lo + i;
		if (n == 0 || n > FILTER_MAX_SET)
			return false;
		f->op = p->op ? SSF_NOTIN : SSF_IN;
		f->nset = n;
		f->set = set;
		return true;
	}
}

static void
filter_init(AggExec *ex)
{
	SiftAggSpec *s = ex->spec;
	SiftSrcFilter f[SIFT_COLSRC_MAX_FILTERS];
	int			at[SIFT_COLSRC_MAX_FILTERS],
				n = 0;
	uint64		ok;

	ex->pushed = MemoryContextAllocZero(ex->cxt, Max(s->npred, 1) * sizeof(bool));
	ex->pflt = MemoryContextAllocZero(ex->cxt, Max(s->npred, 1) * sizeof(SiftSrcFilter));
	for (int i = 0; i < s->npred && n < SIFT_COLSRC_MAX_FILTERS; i++)
	{
		SiftAggPred *p = s->pred[i];

		if (p->level != 0 || p->late || p->leaf >= ex->nbase || (p->kind != SAP_CMP && p->kind != SAP_IN))
			continue;
		memset(&f[n], 0, sizeof(SiftSrcFilter));
		if (int_filter(ex, p, &f[n]))
			at[n++] = i;
	}
	/* the source tests the stored rows of a unit, the rows of a match set may be renumbered: with a search the executor tests them */
	ok = n > 0 && s->search == NULL ? sift_colsrc_set_filters(ex->src, f, n) : 0;
	for (int k = 0; k < n; k++)
		if (ok >> k & 1)
		{
			ex->pushed[at[k]] = true;
			ex->pflt[at[k]] = f[k];
			ex->nfilter++;
		}
}

/* the leaves the source reads again from the heap for the rows that hold a value they cannot store */
static void
recheck_init(AggExec *ex)
{
	SiftAggSpec *s = ex->spec;

	for (int i = 0; i < s->nleaves; i++)
		if (s->recheck[i] != SRK_NONE)
		{
			sift_colsrc_set_recheck(ex->src, s->leaves[i], s->recheck[i]);
			ex->recheck = true;
		}
}

/*
 * The conjuncts on the JSON type of a leaf's path (SAP_TYPE) are the source's: it keeps the rows of the types and raises for one whose value
 * it cannot give. The other rows may hold such a value in this leaf, which its reads then give as NULL.
 */
static void
type_init(AggExec *ex)
{
	for (int i = 0; i < ex->spec->npred; i++)
		if (ex->spec->pred[i]->kind == SAP_TYPE)
		{
			sift_colsrc_tolerate_ovf(ex->src, ex->spec->pred[i]->leaf);
			ex->pushed[i] = true;
			ex->ntype++;
		}
}

/*
 * The conjuncts on wide leaves (SAP_WIDE) are the source's: their operands are evaluated once, as Datums of the leaf's type (an IN list as the
 * elements of its array, a NULL operand as isnull), and the source returns the rows that pass test k of wide_filters.
 */
static void
wide_operand(AggExec *ex, const SiftAggPred *p, SiftSrcWideTest *t)
{
	bool		isnull;
	ExprState  *e;
	Datum		d;
	Datum	   *vals = palloc(sizeof(Datum));
	bool	   *nulls = palloc(sizeof(bool));
	int16		len;
	bool		byval;
	char		align;

	if (p->op == SWT_ISNULL || p->op == SWT_NOTNULL)
		return;
	e = ExecInitExpr((Expr *) p->expr, EXPR_PARENT(ex));
	d = ExecEvalExprSwitchContext(e, ex->lutcx, &isnull);
	get_typlenbyvalalign(p->argtype, &len, &byval, &align);
	t->nvals = 1;
	nulls[0] = isnull;
	if (isnull)
		vals[0] = (Datum) 0;
	else if (p->op == SWT_IN || p->op == SWT_NOTIN)
		deconstruct_array(DatumGetArrayTypePCopy(d), p->argtype, len, byval, align, &vals, &nulls, &t->nvals);
	else
		vals[0] = datumCopy(d, byval, len);
	t->vals = vals;
	t->isnull = nulls;
}

static void
wide_init(AggExec *ex)
{
	SiftAggSpec *s = ex->spec;
	SiftSrcWideTest t[SIFT_AGG_MAX_WIDE];
	MemoryContext old = MemoryContextSwitchTo(ex->cxt);
	int			n = 0;

	for (int i = 0; i < s->npred; i++)
		if (s->pred[i]->kind == SAP_WIDE)
		{
			t[n] = (SiftSrcWideTest) {.leaf = s->pred[i]->leaf, .op = s->pred[i]->op};
			wide_operand(ex, s->pred[i], &t[n++]);
			ex->pushed[i] = true;
		}
	if (n > 0 && sift_colsrc_set_wide(ex->src, t, n) != n)
		elog(ERROR, "sift: the source did not take the conjuncts on wide leaves");
	ex->nwide = n;
	MemoryContextSwitchTo(old);
}

/* The TID range of the SAP_TID conjuncts: the scan reads the superblocks it touches, and a heap's edge rows are looked up for their visible version */
static void
tid_init(AggExec *ex, Snapshot snapshot)
{
	SiftAggSpec *s = ex->spec;
	MemoryContext old = MemoryContextSwitchTo(ex->cxt);

	ex->tid_pred = -1;
	ex->tid_sb_hi = PG_UINT32_MAX;
	for (int i = 0; i < s->npred && ex->tid_pred < 0; i++)
		if (s->pred[i]->kind == SAP_TID)
			ex->tid_pred = s->pred[i]->id;
	if (ex->tid_pred >= 0)
	{
		sift_aggspec_tid_range(s, &ex->tid_lo, &ex->tid_hi);
		ex->tid_sb_hi = 0;
		ex->tid_rows = palloc(VEC * sizeof(uint32));
		ex->tid_tids = palloc(VEC * sizeof(ItemPointerData));
		if (ex->tid_lo < ex->tid_hi)
		{
			ex->tid_blo = ex->tid_lo >> 16;
			ex->tid_bhi = (ex->tid_hi - 1) >> 16;
			ex->tid_sb_lo = ex->tid_blo / SIFT_SB_PAGES;
			ex->tid_sb_hi = ex->tid_bhi / SIFT_SB_PAGES + 1;
		}
		if (!ex->lay->is_table)
		{
			ex->tid_vis = palloc(sizeof(SiftVis));
			sift_vis_begin(ex->tid_vis, ex->heap, snapshot, NULL, 0);
			ex->tid_slot = table_slot_create(ex->heap, NULL);
		}
	}
	MemoryContextSwitchTo(old);
}

/* what a key keeps for the whole scan: its expression and the type of its input; the ids are the table's (table_init) */
static void
register_key(AggExec *ex, int ki)
{
	SiftAggKey *sk = &ex->spec->key[ki];
	KeyState   *k = &ex->ks[ki];
	const SiftSrcLeaf *lf = key_lf(ex, ki);
	MemoryContext old = MemoryContextSwitchTo(ex->cxt);

	if (sk->kind == SAK_SET)
		k->ident = true;
	else if (sk->kind == SAK_LUT)
	{
		k->expr = init_lut_expr(ex, sk->expr);
		k->param = find_param(sk->expr);
		k->bucket = (int64) sk->bucket * USECS_PER_SEC;
	}
	else if (sk->kind == SAK_NUM)
	{
		k->numk = true;
		idmap_init(&k->im);
	}
	else if (sk->kind == SAK_LEAF)
	{
		/* the leaf itself: the placeholder has the key's type */
		k->param = makeNode(Param);
		k->param->paramkind = PARAM_EXEC;
		k->param->paramtype = sk->typid;
		k->param->paramtypmod = sk->typmod;
		k->param->paramcollid = sk->collid;
		k->ident = lf->vtype == SCV_I32 || lf->vtype == SCV_F64;
		if (lf->vtype == SCV_F64)
			ident_go_wide(ex, k);
	}
	MemoryContextSwitchTo(old);
}

static void
register_combos(AggExec *ex)
{
	SiftAggSpec *s = ex->spec;
	MemoryContext old = MemoryContextSwitchTo(ex->cxt);
	bool		preserved = false;

	for (int j = 0; j < s->njoin; j++)
		preserved |= sift_aggjoin_preserves(s->join[j].kind);

	ex->comcxt = AllocSetContextCreate(ex->cxt, "sift agg combo", ALLOCSET_SMALL_SIZES);
	ex->cs = palloc0(Max(s->ncombo, 1) * sizeof(ComboSt));
	for (int ci = 0; ci < s->ncombo; ci++)
	{
		SiftAggCombo *cb = &s->combo[ci];
		ComboSt    *cs = &ex->cs[ci];

		cs->ninputs = Max(cb->nleaves, 1);
		cs->cxt = AllocSetContextCreate(ex->cxt, "sift agg combo cache", ALLOCSET_DEFAULT_SIZES);
		cs->resettable = cb->join < 0;
		cs->by_id = !preserved && cb->join < 0 && cb->nleaves == 1 && cb->leaf[0] >= 0 && leaf_wide(&ex->lay->leaf[cb->leaf[0]]);
		cs->slot = palloc(Max(cb->nslots, 1) * sizeof(ExprState *));
		cs->tlen = palloc0(Max(cb->nslots, 1) * sizeof(int16));
		cs->tbyval = palloc0(Max(cb->nslots, 1) * sizeof(bool));
		for (int sl = 0; sl < cb->nslots; sl++)
		{
			cs->slot[sl] = ExecInitExpr((Expr *) cb->slot[sl].expr, EXPR_PARENT(ex));
			get_typlenbyval(exprType(cb->slot[sl].expr), &cs->tlen[sl], &cs->tbyval[sl]);
		}
		for (int p = 0; p < cb->nleaves; p++)
		{
			const SiftSrcLeaf *lf;

			if (cb->leaf[p] < 0)
			{
				cs->resettable = false;
				continue;
			}
			lf = &ex->lay->leaf[cb->leaf[p]];

			cs->resettable &= lf->vtype != SCV_CODE;
			if (lf->typid == NUMERICOID && lf->vtype == SCV_I64 && lf->scale > 0)
				cs->unit[p] = DirectFunctionCall3(numeric_in, CStringGetDatum(psprintf("0.%0*d", lf->scale, 1)), ObjectIdGetDatum(InvalidOid),
												  Int32GetDatum(-1));
		}
		for (int ai = 0; ai < s->naggs; ai++)
			if ((s->agg[ai].kind == SAA_X_SUM || s->agg[ai].kind == SAA_X_AVG) && s->agg[ai].restype == NUMERICOID && s->agg[ai].combo == ci)
			{
				if (!cs->decode)
					cs->decode = palloc0(Max(cb->nslots, 1) * sizeof(bool));
				cs->decode[s->agg[ai].slot] = true;
			}
		combo_reset(ex, ci);
		if (cb->join >= 0)
			foreach_ptr(Node, arg, s->join[cb->join].args)
				cs->args = lappend(cs->args, ExecInitExpr((Expr *) arg, EXPR_PARENT(ex)));
		for (int k = 0; cb->join >= 0 && k < s->join[cb->join].nkey; k++)
			if (s->join[cb->join].keyexpr[k])
				cs->kx[k] = ExecInitExpr((Expr *) s->join[cb->join].keyexpr[k], EXPR_PARENT(ex));
		ex->clbase[ci] = ex->ncl;
		ex->ncl += cb->nleaves;
		if (cb->join >= 0)
			ex->jcombo[ex->nj++] = ci;
	}
	MemoryContextSwitchTo(old);
}

/*
 * A tuple of an inner or semi join needs a row of the table with its key, so the keys of the loaded table are a conjunct of the scan,
 * `leaf IN (keys)`: the leaf's zones, the source's filters and the row bitmaps apply it before a tuple is made, where the join would
 * resolve every tuple through the combo cache. The table is loaded here (the table's own conditions already hold for its rows).
 */
static bool
join_filter_leaf(AggExec *ex, int ci, int k, int *leaf)
{
	const SiftAggCombo *cb = &ex->spec->combo[ci];
	const SiftAggJoin *j = &ex->spec->join[cb->join];
	const SiftSrcLeaf *lf;

	if ((j->kind != SJ_INNER && j->kind != SJ_SEMI) || j->params != NIL || j->keypos[k] < 0 || cb->level != 0)
		return false;
	*leaf = cb->leaf[j->keypos[k]];
	if (*leaf < 0 || *leaf >= ex->nbase)
		return false;
	lf = &ex->lay->leaf[*leaf];
	return lf->kind == SCL_SCALAR && lf->parent < 0 && lf->vtype == SCV_I32;
}

static SiftAggPred *
join_filter_pred(AggExec *ex, int ci, int k, int leaf, int id)
{
	const SiftAggCombo *cb = &ex->spec->combo[ci];
	const SiftAggJoin *j = &ex->spec->join[cb->join];
	const MapTab *m = ex->cs[ci].map;
	PredState  *st = &ex->ps[id];
	SiftAggPred *p;
	int64	   *vals = palloc(Max(m->nrow, 1) * sizeof(int64));
	int64		lo = PG_INT64_MAX,
				hi = PG_INT64_MIN;
	int			nv = 0;

	for (uint32 r = 0; r < m->nrow; r++)
	{
		int64		v = m->kint[(size_t) r * m->nkey + k];

		if (v < PG_INT32_MIN || v > PG_INT32_MAX)
			continue;				/* no int4 value equals it */
		vals[nv++] = v;
		lo = Min(lo, v);
		hi = Max(hi, v);
	}
	if (!in_build(st, false, true, vals, nv, lo, hi))
	{
		pfree(vals);
		return NULL;
	}
	pfree(vals);
	st->ready = true;
	p = palloc0(sizeof(SiftAggPred));
	p->kind = SAP_IN;
	p->id = id;
	p->leaf = leaf;
	p->leaf2 = -1;
	p->argtype = ex->lay->leaf[leaf].typid;
	p->desc = psprintf("%s IN (keys of %s)", ex->lay->leaf[leaf].name, j->desc);
	return p;
}

static void
join_filters(AggExec *ex, MemoryContext caller)
{
	SiftAggSpec *s = ex->spec;
	MemoryContext old = MemoryContextSwitchTo(ex->cxt);
	List	   *add = NIL;
	int			total = s->npreds_total;

	for (int j = 0; j < s->njoin; j++)
		if (sift_aggjoin_preserves(s->join[j].kind))
			goto done;
	if (!sift_agg_join_filter || s->topn)
		goto done;
	for (int ci = 0; ci < s->ncombo; ci++)
	{
		const SiftAggJoin *j;
		int			leaf;

		if (s->combo[ci].join < 0)
			continue;
		j = &s->join[s->combo[ci].join];
		for (int k = 0; k < j->nkey; k++)
		{
			SiftAggPred *p;

			if (!join_filter_leaf(ex, ci, k, &leaf))
				continue;
			if (ex->cs[ci].map == NULL)
			{
				/* a child plan runs in the context the scan was started in */
				MemoryContextSwitchTo(caller);
				ex->cs[ci].map = map_load(ex, ci);
				MemoryContextSwitchTo(ex->cxt);
			}
			if (ex->cs[ci].map->ktext[k])
				continue;
			/* the table of the predicate is the state of the id it is given: make room first */
			ex->ps = repalloc(ex->ps, (total + 1) * sizeof(PredState));
			memset(&ex->ps[total], 0, sizeof(PredState));
			if ((p = join_filter_pred(ex, ci, k, leaf, total)) != NULL)
			{
				add = lappend(add, p);
				total++;
			}
		}
	}
	if (add != NIL)
	{
		SiftAggSpec *c = palloc(sizeof(SiftAggSpec));

		*c = *s;
		c->pred = palloc((s->npred + list_length(add)) * sizeof(SiftAggPred *));
		memcpy(c->pred, s->pred, s->npred * sizeof(SiftAggPred *));
		foreach_ptr(SiftAggPred, p, add)
			c->pred[c->npred++] = p;
		c->npreds_total = total;
		ex->spec = c;
		ex->njfilter = list_length(add);
	}
done:
	MemoryContextSwitchTo(old);
}

/* An empty group table, all in tcxt: key ids, group arrays, count(DISTINCT) sets */
static void
table_init(AggExec *ex)
{
	SiftAggSpec *s = ex->spec;
	MemoryContext old = MemoryContextSwitchTo(ex->tcxt);
	uint64		total = 1;

	ex->hash = false;
	ex->direct_max = ex->lanes || ex->lane_serial ? LANE_DIRECT_MAX : DIRECT_MAX;
	ex->ngroups = 0;
	ex->hkey = NULL;
	ex->hkcap = 0;
	ex->hslot = NULL;
	ex->nhslot = 0;
	ex->row_base = 0;
	for (int j = 0; j < s->nkeys; j++)
	{
		KeyState   *k = &ex->ks[j];
		KeyState	keep = {.expr = k->expr, .param = k->param, .ident = k->ident, .numk = k->numk, .bucket = k->bucket};

		*k = keep;
		ot_init(&k->ot, s->key[j].eqop, s->key[j].typid, s->key[j].collid);
		rt_init(&k->rt, sizeof(uint32));
		k->cap = 4;
		k->nullid = UINT32_MAX;
		if (s->key[j].kind == SAK_SET)
			for (int i = 0; i < s->nsets; i++)
				ident_add(ex, k, i);
		else if (k->numk)
			idmap_init(&k->im);
		else if ((s->key[j].kind == SAK_LEAF && key_lf(ex, j)->vtype == SCV_F64) || k->bucket)
			ident_go_wide(ex, k);
	}
	/* room for the groups the planner expects of one key, instead of doubling the arrays as the ids come */
	if (s->nkeys == 1 && !ex->lanes && s->est_groups > PRESIZE_MIN && s->est_groups <= ex->direct_max)
		ex->ks[0].cap = pg_nextpower2_32((uint32) s->est_groups);
	for (int j = 0; j < s->nkeys; j++)
		total = Min((uint64) ex->direct_max + 1, total * ex->ks[j].cap);
	if (total > ex->direct_max)
	{
		hash_init(ex);
		total = 1024;
	}
	else
		direct_layout(ex);
	ex->gcap = total;
	ex->seen = palloc0(total * sizeof(int64));
	ex->acc = acc_alloc(ex, total);
	arms_resize(ex, total, NULL, 0);
	ex->dset = palloc0(Max(s->naggs, 1) * sizeof(DSet));
	ex->dist = palloc0(Max(s->naggs, 1) * sizeof(Dist *));
	ex->aot = palloc0(Max(s->naggs, 1) * sizeof(OutTable));
	ex->amax = palloc0(Max(s->naggs, 1) * sizeof(double));
	ex->amap = palloc0(Max(s->naggs, 1) * sizeof(IdMap));
	ex->osa = palloc0(Max(s->naggs, 1) * sizeof(Osa *));
	ex->oval = palloc0(Max(s->naggs, 1) * sizeof(double *));
	ex->xcmp = palloc0(Max(s->naggs, 1) * sizeof(FmgrInfo *));
	ex->xlen = palloc0(Max(s->naggs, 1) * sizeof(int16));
	ex->xbyval = palloc0(Max(s->naggs, 1) * sizeof(bool));
	for (int i = 0; i < s->naggs; i++)
	{
		SiftAggAgg *a = &s->agg[i];
		TypeCacheEntry *tc;

		if (a->kind == SAA_X_MIN || a->kind == SAA_X_MAX)
		{
			ex->xcmp[i] = &lookup_type_cache(a->argtype, TYPECACHE_CMP_PROC_FINFO)->cmp_proc_finfo;
			get_typlenbyval(a->argtype, &ex->xlen[i], &ex->xbyval[i]);
		}
		if (is_ordered_agg(a))
			ex->osa[i] = a->share == i ? osa_new(ex) : ex->osa[a->share];
		if (a->num.n && (a->kind == SAA_COUNT_DISTINCT || a->distinct))
			idmap_init(&ex->amap[i]);
		if (a->kind == SAA_GEN)
		{
			/* the hash set of a DISTINCT aggregate holds the byte images of its values; so does the table of the values of a joined table */
			for (int k = 0; k < a->gen->nargs; k++)
				if (a->gen->arg[k].mapped)
					ot_init_image(&ex->aot[i], a->gen->arg[k].typid);
			if (a->gen->distinct && !a->gen->sorted)
				ot_init_image(&ex->aot[i], a->gen->arg[a->gen->darg].typid);
			continue;
		}
		if (a->kind == SAA_COUNT_DISTINCT)
			ex->dist[i] = dist_new(ex, !a->num.n && a->leaf >= 0 && ex->lay->leaf[a->leaf].vtype != SCV_CODE);
		if ((a->kind >= SAA_M_COUNT && a->kind <= SAA_M_AVG) || a->kind == SAA_M_PG)
			ex->dset[i].counted = a->kind == SAA_M_SUM || a->kind == SAA_M_AVG || a->kind == SAA_M_PG;
		else if (a->kind != SAA_COUNT_DISTINCT || a->leaf < 0 || ex->lay->leaf[a->leaf].vtype != SCV_CODE)
			continue;
		tc = lookup_type_cache(a->argtype, TYPECACHE_EQ_OPR);
		ot_init(&ex->aot[i], tc->eq_opr, a->argtype, a->collid);
	}
	MemoryContextSwitchTo(old);
}

/* forgets every group (the dictionary maps hold key ids of the table) */
void
table_reset(AggExec *ex)
{
	int			nk = Max(ex->spec->nkeys + ex->spec->naggs, 1);

	for (int i = 0; i < NDICT_SLOTS; i++)
		if (ex->dslot[i].kmap)
			memset(ex->dslot[i].kmap, 0, nk * sizeof(uint32 *));
	dist_end_all(ex);
	MemoryContextReset(ex->tcxt);
	/* the output rows of a combo hold ids of the key tables just forgotten */
	for (int ci = 0; ci < ex->spec->ncombo; ci++)
	{
		ComboSt    *cs = &ex->cs[ci];

		/* the direct table forgets the ids of the entries only: it can be megabytes */
		for (uint32 x = 0; cs->dir && x < cs->nent; x++)
			if (!cs->null_input[x] && cs->vid[x] < cs->dircap)
				cs->dir[cs->vid[x]] = 0;
		cs->nent = cs->nout = 0;
		memset(cs->ht, 0, cs->nht * sizeof(uint32));
	}
	table_init(ex);
}

static void
scan_buffers(AggExec *ex)
{
	SiftAggSpec *s = ex->spec;
	int			maxv = VEC + PADW;

	arms_init(ex);

	ex->t0.sel = MemoryContextAlloc(ex->cxt, (maxv + 16) * sizeof(uint32));
	ex->t0.tidx = MemoryContextAlloc(ex->cxt, maxv * sizeof(uint32));
	ex->t0.ridx = MemoryContextAlloc(ex->cxt, maxv * sizeof(uint32));
	for (int x = 0; x < ex->nj; x++)
	{
		Tup		   *st = &ex->stage[x];

		st->sel = MemoryContextAlloc(ex->cxt, maxv * sizeof(uint32));
		st->tidx = MemoryContextAlloc(ex->cxt, maxv * sizeof(uint32));
		st->ridx = MemoryContextAlloc(ex->cxt, maxv * sizeof(uint32));
		for (int y = 0; y <= x; y++)
			st->cout[ex->jcombo[y]] = MemoryContextAlloc(ex->cxt, maxv * sizeof(uint32));
		ex->seid[x] = MemoryContextAlloc(ex->cxt, maxv * sizeof(uint32));
	}
	for (int p = 0; p < SIFT_AGG_MAX_CLEAVES; p++)
		ex->cvid[p] = MemoryContextAlloc(ex->cxt, maxv * sizeof(uint64));
	for (int ci = 0; ci < s->ncombo; ci++)
	{
		ex->cfree[ci] = MemoryContextAlloc(ex->cxt, maxv * sizeof(uint32));
		if (s->combo[ci].join >= 0)
		{
			if (ex->cs[ci].map == NULL)
				ex->cs[ci].map = map_load(ex, ci);
			if (s->join[s->combo[ci].join].params == NIL)
				semi_lut_build(ex, ci);
		}
	}
	ex->calive = MemoryContextAlloc(ex->cxt, maxv * sizeof(uint32));
	ex->peid = MemoryContextAlloc(ex->cxt, maxv * sizeof(uint32));
	ex->pti = MemoryContextAlloc(ex->cxt, maxv * sizeof(uint32));
	ex->pri = MemoryContextAlloc(ex->cxt, maxv * sizeof(uint32));
	ex->gid = MemoryContextAlloc(ex->cxt, maxv * sizeof(uint32));
	ex->kids = MemoryContextAlloc(ex->cxt, (Size) maxv * Max(s->nkeys, 1) * sizeof(uint32));
	ex->cu32 = MemoryContextAlloc(ex->cxt, maxv * sizeof(uint32));
	ex->c64 = MemoryContextAlloc(ex->cxt, maxv * sizeof(uint64));
	ex->okb = MemoryContextAlloc(ex->cxt, maxv);
	ex->fwin = MemoryContextAllocZero(ex->cxt, maxv);
	ex->nb = MemoryContextAlloc(ex->cxt, maxv);
	ex->vstk = MemoryContextAlloc(ex->cxt, (Size) SIFT_AGG_MAX_VX * maxv * sizeof(double));
	ex->pix = MemoryContextAlloc(ex->cxt, maxv * sizeof(uint32));
	ex->okm = MemoryContextAlloc(ex->cxt, (VECW + PADW) * sizeof(uint64));
	ex->tmpm = MemoryContextAlloc(ex->cxt, (VECW + PADW) * sizeof(uint64));
	ex->tmpm2 = MemoryContextAlloc(ex->cxt, (VECW + PADW) * sizeof(uint64));
	ex->tmpu32 = MemoryContextAlloc(ex->cxt, (Size) maxv * Max(s->nkeys, 1) * sizeof(uint32));
}

/* ---------- partial group tables ---------- */

static void
bw_put(StringInfo b, const void *p, Size n)
{
	appendBinaryStringInfo(b, p, n);
	while (b->len % 8)
		appendStringInfoChar(b, 0);
}

static const void *
br_get(const char **p, Size n)
{
	const void *r = *p;

	*p += TYPEALIGN(8, n);
	return r;
}

/* the aggregate arrays a state holds per group, besides the count: what acc_alloc made */
static void
bw_acc(StringInfo b, const AggAcc *acc, const uint32 *gl, uint32 ng)
{
	char	   *tmp = palloc(Max(ng, 1) * sizeof(sift_i128));
	struct
	{
		const void *src;
		int			esize;
	}			col[] = {{acc->n, 8}, {acc->i, 8}, {acc->d, 8}, {acc->nf, 1}, {acc->w, sizeof(sift_i128)}};

	for (int c = 0; c < lengthof(col); c++)
	{
		if (col[c].src == NULL)
			continue;
		for (uint32 r = 0; r < ng; r++)
			memcpy(tmp + (Size) r * col[c].esize, (const char *) col[c].src + (Size) gl[r] * col[c].esize, col[c].esize);
		bw_put(b, tmp, (Size) ng * col[c].esize);
	}
	pfree(tmp);
}

/* the values of an output table whose ids are listed in old[0..nused), as datums */
static void
bw_values(StringInfo b, const OutTable *t, const uint32 *old, uint32 nused)
{
	StringInfoData vb;
	uint32		hd[2];

	initStringInfo(&vb);
	for (uint32 i = 0; i < nused; i++)
	{
		Datum		d = t->val[old[i] - 1];
		Size		sz = datumEstimateSpace(d, false, t->byval, t->len);
		char	   *at;

		enlargeStringInfo(&vb, sz);
		at = vb.data + vb.len;
		datumSerialize(d, false, t->byval, t->len, &at);
		vb.len += sz;
	}
	hd[0] = nused;
	hd[1] = vb.len;
	bw_put(b, hd, sizeof(hd));
	if (vb.len)
		bw_put(b, vb.data, vb.len);
	pfree(vb.data);
}

/* renumbers the ids ids[0..n) from 1 in order of use (0 stays 0); returns the old ids by new id */
static uint32 *
renumber(uint32 *ids, uint32 n, uint32 nold, uint32 *nused)
{
	uint32	   *rm = palloc0((nold + 1) * sizeof(uint32)),
			   *old;

	*nused = 0;
	for (uint32 r = 0; r < n; r++)
		if (ids[r] && !rm[ids[r]])
			rm[ids[r]] = ++*nused;
	old = palloc(Max(*nused, 1) * sizeof(uint32));
	for (uint32 id = 1; id <= nold; id++)
		if (rm[id])
			old[rm[id] - 1] = id;
	for (uint32 r = 0; r < n; r++)
		ids[r] = ids[r] ? rm[ids[r]] : 0;
	pfree(rm);
	return old;
}

static uint32
group_key_id(AggExec *ex, int j, uint32 g)
{
	return ex->hash ? ex->hkey[(Size) g * ex->spec->nkeys + j] : (g / ex->ks[j].mult) % ex->ks[j].cap;
}

static void
blob_key(AggExec *ex, int j, const uint32 *gl, uint32 ng, StringInfo b)
{
	KeyState   *k = &ex->ks[j];
	uint32	   *ids = palloc(Max(ng, 1) * sizeof(uint32)),
			   *old,
				nused;

	for (uint32 r = 0; r < ng; r++)
		ids[r] = group_key_id(ex, j, gl[r]);
	old = renumber(ids, ng, k->numk ? k->im.n : k->ident ? k->nident : k->ot.n, &nused);
	if (k->ident || k->numk)
	{
		/* the values themselves: 64 bits for an integer or the bits of a float, 128 for a numeric */
		Size		esz = k->numk ? sizeof(sift_i128) : sizeof(int64);
		uint32		hd[2] = {nused, nused * esz};
		char	   *vals = palloc(Max(nused, 1) * esz);

		for (uint32 i = 0; i < nused; i++)
			if (k->numk)
				memcpy(vals + i * esz, &k->im.val[old[i] - 1], esz);
			else
				memcpy(vals + i * esz, &k->idval[old[i] - 1], esz);
		bw_put(b, hd, sizeof(hd));
		bw_put(b, vals, nused * esz);
	}
	else
		bw_values(b, &k->ot, old, nused);
	bw_put(b, ids, (Size) ng * sizeof(uint32));
}

/*
 * The set of a count(DISTINCT) of aggregate ai in the groups of cidx (blob numbering), header {pairs, chunks}: the (group, value)
 * pairs, then the (group, chunk number) of each bitmap chunk and its bits. A set that is still a bitmap of raw values sends the chunks
 * with DB_DENSE bits or more as they are, so that the leader ORs them and counts by popcount, and the bits of the others as pairs;
 * what it sends is bounded by the memory of the bitmap, hash_mem. The values of a dictionary leaf go by value.
 */
static void
blob_set(AggExec *ex, int ai, const uint32 *cidx, StringInfo b)
{
	Dist	   *d = ex->dist[ai];
	bool		dict = ex->lay->leaf[ex->spec->agg[ai].leaf].vtype == SCV_CODE;
	uint32		np,
			   *pg,
			   *pv,
				hd[2],
			   *old = NULL,
				nused = 0,
				nc = 0,
				none[1] = {0},
			   *cg = none,
			   *cc = none;
	const uint64 **bits = NULL;

	if (d->mode == DD_BITS && d->sgn)
	{
		uint32		n = Min(ex->hash ? ex->ngroups : ex->gcap, d->ngd);
		uint64		total = 0;

		for (uint32 g = 0; g < n; g++)
			if (cidx[g] != UINT32_MAX)
				total += ex->acc[ai].n[g];
		pg = palloc(Max(total, 1) * sizeof(uint32));
		pv = palloc(Max(total, 1) * sizeof(uint32));
		cg = palloc(Max(total / DB_DENSE, 1) * sizeof(uint32));
		cc = palloc(Max(total / DB_DENSE, 1) * sizeof(uint32));
		bits = palloc(Max(total / DB_DENSE, 1) * sizeof(uint64 *));
		np = 0;
		for (uint32 g = 0; g < n; g++)
			for (uint32 c = 0; d->gd[g] && cidx[g] != UINT32_MAX && c < d->ncd; c++)
			{
				const uint64 *ch = d->gd[g][c];
				int			pop = 0;

				for (int w = 0; ch && w < DB_WORDS; w++)
					pop += pg_popcount64(ch[w]);
				if (pop >= DB_DENSE)
				{
					cg[nc] = cidx[g];
					cc[nc] = (uint32) (d->cbase + c);
					bits[nc++] = ch;
				}
				else if (pop > 0)
					for (int w = 0; w < DB_WORDS; w++)
						for (uint64 m = ch[w]; m; m &= m - 1)
						{
							pg[np] = cidx[g];
							pv[np++] = (uint32) ((d->cbase + c) * (1 << DB_SHIFT) + w * 64 + pg_rightmost_one_pos64(m));
						}
			}
	}
	else
		dist_pairs(ex, ai, cidx, &pg, &pv, &np);
	if (dict)
		old = renumber(pv, np, ex->aot[ai].n, &nused);
	hd[0] = np;
	hd[1] = nc;
	bw_put(b, hd, sizeof(hd));
	bw_put(b, pg, (Size) np * sizeof(uint32));
	bw_put(b, pv, (Size) np * sizeof(uint32));
	bw_put(b, cg, (Size) nc * sizeof(uint32));
	bw_put(b, cc, (Size) nc * sizeof(uint32));
	for (uint32 i = 0; i < nc; i++)
		appendBinaryStringInfo(b, (const char *) bits[i], DB_WORDS * sizeof(uint64));
	if (dict)
		bw_values(b, &ex->aot[ai], old, nused);
}

typedef struct OsaRec
{
	uint32		g;				/* group of the blob */
	uint32		pad;
	uint64		key;
	uint64		cnt;
} OsaRec;

/* the values of an ordered-set aggregate that owns them, as runs (group of the blob, value, count): the histogram or the sorted tuples */
static void
blob_osa(AggExec *ex, int ai, const uint32 *cidx, uint32 n, StringInfo b)
{
	Osa		   *o = ex->osa[ai];
	AggAcc	   *own = &ex->acc[ai];
	uint32	   *sid2g = palloc(Max(o->nsid, 1) * sizeof(uint32)),
				hd[2] = {0, 0};
	StringInfoData rb;
	OsaRun		r;

	for (uint32 g = 0; g < n; g++)
		if (own->i[g])
			sid2g[own->i[g] - 1] = cidx[g];
	initStringInfo(&rb);
	if (o->sort)
	{
		osa_sort(o);
		while (osa_next(o, &r))
		{
			OsaRec		rec = {sid2g[r.sid], 0, r.key, r.cnt};

			appendBinaryStringInfo(&rb, (const char *) &rec, sizeof(rec));
			hd[0]++;
		}
		tuplesort_end(o->sort);
		o->sort = NULL;
	}
	else
		for (uint64 i = 0; i < o->cap; i++)
			if (o->slot[i].sid1)
			{
				OsaRec		rec = {sid2g[o->slot[i].sid1 - 1], 0, o->slot[i].key, o->slot[i].cnt};

				appendBinaryStringInfo(&rb, (const char *) &rec, sizeof(rec));
				hd[0]++;
			}
	bw_put(b, hd, sizeof(hd));
	bw_put(b, rb.data, rb.len);
	pfree(rb.data);
	pfree(sid2g);
}

/* the partial states of the generic aggregate ai, one record per group of gl */
static void
blob_gen(AggExec *ex, int ai, const uint32 *gl, uint32 ng, StringInfo b)
{
	StringInfoData vb;
	uint32		hd[2];
	MemoryContext old = MemoryContextSwitchTo(ex->lutcx->ecxt_per_tuple_memory);

	initStringInfo(&vb);
	for (uint32 r = 0; r < ng; r++)
		gen_state_put(ex, ex->gst[ai], &ex->acc[ai], gl[r], &vb);
	MemoryContextSwitchTo(old);
	hd[0] = ng;
	hd[1] = vb.len;
	bw_put(b, hd, sizeof(hd));
	bw_put(b, vb.data, vb.len);
	pfree(vb.data);
	ResetExprContext(ex->lutcx);
}

static void
blob_dump(AggExec *ex, int lane, StringInfo b)
{
	SiftAggSpec *s = ex->spec;
	uint32		n = ex->hash ? ex->ngroups : ex->gcap,
				ng = 0,
			   *gl = palloc(Max(n, 1) * sizeof(uint32)),
			   *cidx = palloc(Max(n, 1) * sizeof(uint32));
	BlobHdr		h = {BLOB_MAGIC, lane, 0, 0};

	dist_finish_all(ex);
	for (uint32 g = 0; g < n; g++)
	{
		cidx[g] = UINT32_MAX;
		if (ex->seen[g] > 0)
		{
			cidx[g] = ng;
			gl[ng++] = g;
		}
	}
	h.ngroups = ng;
	bw_put(b, &h, sizeof(h));
	for (int j = 0; j < s->nkeys; j++)
		blob_key(ex, j, gl, ng, b);
	{
		AggAcc		seen = {.n = ex->seen};

		bw_acc(b, &seen, gl, ng);
	}
	for (int i = 0; i < s->naggs; i++)
	{
		bw_acc(b, &ex->acc[i], gl, ng);
		if (s->agg[i].kind == SAA_COUNT_DISTINCT)
			blob_set(ex, i, cidx, b);
		if (is_ordered_agg(&s->agg[i]) && s->agg[i].share == i)
			blob_osa(ex, i, cidx, n, b);
		if (s->agg[i].kind == SAA_GEN)
			blob_gen(ex, i, gl, ng, b);
	}
	bw_put(b, ex->amax, Max(s->naggs, 1) * sizeof(double));
	dist_end_all(ex);
	pfree(gl);
	pfree(cidx);
}

/* ids of the values of an integer key in the table (the ids of the range table, or of the wide one) */
static void
merge_ident(AggExec *g, int j, const int64 *vals, uint32 nv, uint32 *xid)
{
	KeyState   *k = &g->ks[j];
	int64		mn = PG_INT64_MAX,
				mx = PG_INT64_MIN;

	for (uint32 i = 0; i < nv; i++)
	{
		mn = Min(mn, vals[i]);
		mx = Max(mx, vals[i]);
	}
	if (nv == 0)
		return;
	if (k->wcap || mn < PG_INT32_MIN || mx > PG_INT32_MAX || rt_span(&k->rt, (int32) mn, (int32) mx) > MAX_RANGE)
	{
		if (!k->wcap)
			ident_go_wide(g, k);
		for (uint32 i = 0; i < nv; i++)
			xid[i + 1] = ident_wide_id(g, k, vals[i]);
	}
	else
	{
		KeyFill		f = {g, j};
		MemoryContext old = MemoryContextSwitchTo(g->tcxt);

		rt_cover(&k->rt, (int32) mn, (int32) mx, key_range_fill, &f);
		MemoryContextSwitchTo(old);
		for (uint32 i = 0; i < nv; i++)
			xid[i + 1] = ((uint32 *) k->rt.tab)[(int32) vals[i] - k->rt.lo];
	}
}

/* xid[1..] = the ids in table t of the values a blob lists; the reader is left after them */
static void
merge_values(AggExec *g, OutTable *t, const char **p, uint32 *xid)
{
	const uint32 *hd = br_get(p, 8);
	const char *at = *p;

	for (uint32 i = 0; i < hd[0]; i++)
	{
		bool		isnull;
		Datum		d = datumRestore((char **) &at, &isnull);
		MemoryContext old = MemoryContextSwitchTo(g->tcxt);

		xid[i + 1] = ot_id(t, d);
		MemoryContextSwitchTo(old);
	}
	*p += TYPEALIGN(8, hd[1]);
}

typedef struct BlobAgg
{
	AggAcc		a;				/* the arrays of the blob, read only */
	uint32		npairs;
	const uint32 *pg;
	const uint32 *pv;
	uint32		nchunks;		/* count(DISTINCT) bitmap chunks: group and number of each, then their bits */
	const uint32 *cg;
	const uint32 *cc;
	const uint64 *bits;
	uint32	   *xv;				/* dictionary values: the ids in the table */
	uint32		nrec;			/* ordered sets: the runs of the values */
	const OsaRec *rec;
	const char *gcur;			/* generic aggregates: the next partial state */
} BlobAgg;

static void
merge_minmax(AggExec *g, int ai, const BlobAgg *ba, uint32 r, uint32 t)
{
	SiftAggAgg *a = &g->spec->agg[ai];
	AggAcc	   *acc = &g->acc[ai];
	bool		ismin = a->kind == SAA_MIN,
				f64 = a->num.n == 0 && g->lay->leaf[a->leaf].vtype == SCV_F64;

	if (ba->a.n[r] == 0)
		return;
	if (a->num.n)
	{
		if (acc->n[t] == 0 || (ismin ? ba->a.w[r] < acc->w[t] : ba->a.w[r] > acc->w[t]))
			acc->w[t] = ba->a.w[r];
		acc->n[t] += ba->a.n[r];
		return;
	}
	if (acc->n[t] == 0)
	{
		acc->i[t] = ba->a.i[r];
		acc->d[t] = ba->a.d[r];
	}
	else if (f64 ? (ismin ? float8_cmp_internal(ba->a.d[r], acc->d[t]) < 0 : float8_cmp_internal(ba->a.d[r], acc->d[t]) > 0) :
			 (ismin ? ba->a.i[r] < acc->i[t] : ba->a.i[r] > acc->i[t]))
	{
		acc->i[t] = ba->a.i[r];
		acc->d[t] = ba->a.d[r];
	}
	acc->n[t] += ba->a.n[r];
}

static void
merge_agg(AggExec *g, int ai, BlobAgg *ba, const uint32 *gids, uint32 r0, uint32 n)
{
	SiftAggAgg *a = &g->spec->agg[ai];
	AggAcc	   *acc = &g->acc[ai];

	for (uint32 r = r0; r < r0 + n; r++)
	{
		uint32		t = gids[r];

		switch (a->kind)
		{
			case SAA_COUNT_STAR:
			case SAA_COUNT:
			case SAA_COUNT_ROWS:
				acc->n[t] += ba->a.n[r];
				break;
			case SAA_SUM_INT:
			case SAA_AVG_INT:
				acc->n[t] += ba->a.n[r];
				acc->i[t] += ba->a.i[r];
				break;
			case SAA_SUM_F64:
			case SAA_AVG_F64:
				acc->n[t] += ba->a.n[r];
				acc->d[t] += ba->a.d[r];
				acc->nf[t] |= ba->a.nf[r];
				break;
			case SAA_MIN:
			case SAA_MAX:
				merge_minmax(g, ai, ba, r, t);
				break;
			case SAA_SUM_NUM:
			case SAA_AVG_NUM:
				acc->n[t] += ba->a.n[r];
				acc->w[t] += ba->a.w[r];
				acc->nf[t] |= ba->a.nf[r];
				break;
			case SAA_COUNT_DISTINCT:
			case SAA_GROUPING:
				break;
			case SAA_PCT_CONT:
			case SAA_PCT_DISC:
				acc->n[t] += ba->a.n[r];
				break;
			case SAA_GEN:
				gen_state_merge(g, g->gst[ai], acc, t, &ba->gcur);
				break;
			default:
				elog(ERROR, "sift: aggregate kind %d has no partial table", a->kind);
		}
	}
}

/* the arrays of a blob's aggregate (the layout of bw_acc), with the pairs of its set */
static void
merge_read_agg(AggExec *g, int ai, const char **p, uint32 ng, BlobAgg *ba)
{
	const AggAcc *acc = &g->acc[ai];

	ba->a.n = (int64 *) br_get(p, (Size) ng * 8);
	if (acc->i)
		ba->a.i = (int64 *) br_get(p, (Size) ng * 8);
	if (acc->d)
		ba->a.d = (double *) br_get(p, (Size) ng * 8);
	if (acc->nf)
		ba->a.nf = (uint8 *) br_get(p, ng);
	if (acc->w)
		ba->a.w = (sift_i128 *) br_get(p, (Size) ng * sizeof(sift_i128));
	if (g->spec->agg[ai].kind == SAA_COUNT_DISTINCT)
	{
		const uint32 *hd = br_get(p, 8);

		ba->npairs = hd[0];
		ba->nchunks = hd[1];
		ba->pg = br_get(p, (Size) hd[0] * 4);
		ba->pv = br_get(p, (Size) hd[0] * 4);
		ba->cg = br_get(p, (Size) hd[1] * 4);
		ba->cc = br_get(p, (Size) hd[1] * 4);
		ba->bits = br_get(p, (Size) hd[1] * DB_WORDS * sizeof(uint64));
		if (g->lay->leaf[g->spec->agg[ai].leaf].vtype == SCV_CODE)
		{
			const uint32 *vh = (const uint32 *) *p;

			ba->xv = palloc0((vh[0] + 1) * sizeof(uint32));
			merge_values(g, &g->aot[ai], p, ba->xv);
		}
	}
	if (is_ordered_agg(&g->spec->agg[ai]) && g->spec->agg[ai].share == ai)
	{
		ba->nrec = *(const uint32 *) br_get(p, 8);
		ba->rec = br_get(p, (Size) ba->nrec * sizeof(OsaRec));
	}
	if (g->spec->agg[ai].kind == SAA_GEN)
	{
		const uint32 *hd = br_get(p, 8);

		ba->gcur = *p;
		*p += TYPEALIGN(8, hd[1]);
	}
}

static void
merge_set(AggExec *g, int ai, const BlobAgg *ba, const uint32 *gids)
{
	for (uint32 i = 0; i < ba->nchunks; i++)
	{
		uint32		t = gids[ba->cg[i]];

		g->acc[ai].n[t] += dist_or_chunk(g, g->dist[ai], t, (int32) ba->cc[i], ba->bits + (Size) i * DB_WORDS);
		g->n_dist_chunks++;
	}
	for (uint32 i = 0; i < ba->npairs; i++)
	{
		uint32		t = gids[ba->pg[i]];

		if (dist_add(g, g->dist[ai], t, ba->xv ? ba->xv[ba->pv[i]] : ba->pv[i]))
			g->acc[ai].n[t]++;
	}
}

static void
merge_osa(AggExec *g, int ai, const BlobAgg *ba, const uint32 *gids)
{
	Osa		   *o = g->osa[ai];
	AggAcc	   *own = &g->acc[ai];

	for (uint32 i = 0; i < ba->nrec; i++)
	{
		uint32		t = gids[ba->rec[i].g];

		if (own->i[t] == 0)
			own->i[t] = ++o->nsid;
		osa_add_n(g, o, own->i[t] - 1, ba->rec[i].key, ba->rec[i].cnt);
	}
}

static void
blob_merge(AggExec *g, const char *p)
{
	SiftAggSpec *s = g->spec;
	const BlobHdr *h = br_get(&p, sizeof(BlobHdr));
	uint32		ng = h->ngroups,
			   *xid[SIFT_AGG_MAX_KEYS],
			   *gids = palloc(Max(ng, 1) * sizeof(uint32));
	const uint32 *lid[SIFT_AGG_MAX_KEYS];
	const int64 *seen;
	BlobAgg    *ba = palloc0(Max(s->naggs, 1) * sizeof(BlobAgg));
	bool		grow = false;

	if (h->magic != BLOB_MAGIC)
		elog(ERROR, "sift: bad partial aggregate");
	for (int j = 0; j < s->nkeys; j++)
	{
		KeyState   *k = &g->ks[j];
		const uint32 *hd = (const uint32 *) p;

		xid[j] = palloc0((hd[0] + 1) * sizeof(uint32));
		if (k->ident)
		{
			const int64 *iv;

			br_get(&p, 8);
			iv = br_get(&p, (Size) hd[0] * sizeof(int64));
			if (s->key[j].kind == SAK_SET)
				for (uint32 i = 0; i < hd[0]; i++)
					xid[j][i + 1] = iv[i] + 1;	/* the ids of the sets are fixed: set + 1 */
			else
				merge_ident(g, j, iv, hd[0], xid[j]);
		}
		else if (k->numk)
		{
			const sift_i128 *nv;

			br_get(&p, 8);
			nv = br_get(&p, (Size) hd[0] * sizeof(sift_i128));
			for (uint32 i = 0; i < hd[0]; i++)
				xid[j][i + 1] = idmap_id(&k->im, nv[i]);
		}
		else
			merge_values(g, &k->ot, &p, xid[j]);
		lid[j] = br_get(&p, (Size) ng * sizeof(uint32));
	}
	seen = br_get(&p, (Size) ng * sizeof(int64));
	for (int i = 0; i < s->naggs; i++)
		merge_read_agg(g, i, &p, ng, &ba[i]);
	if (s->naggs)
		for (int i = 0; i < s->naggs; i++)
			g->amax[i] = Max(g->amax[i], ((const double *) p)[i]);
	if (!g->hash)
		for (int j = 0; j < s->nkeys; j++)
			grow |= key_used(&g->ks[j]) > g->ks[j].cap;
	if (grow)
		direct_grow(g);
	for (uint32 r0 = 0; r0 < ng; r0 += VEC)
	{
		uint32		n = Min(VEC, ng - r0);

		for (uint32 r = r0; r < r0 + n; r++)
		{
			uint32		ids[SIFT_AGG_MAX_KEYS] = {0};

			for (int j = 0; j < s->nkeys; j++)
				ids[j] = xid[j][lid[j][r]];
			if (g->hash)
				gids[r] = hash_group(g, ids);
			else
			{
				gids[r] = 0;
				for (int j = 0; j < s->nkeys; j++)
					gids[r] += ids[j] * g->ks[j].mult;
			}
		}
		if (g->ngroups > g->gcap)
			groups_resize(g, Max(g->gcap * 2, g->ngroups), NULL, g->gcap);
		for (uint32 r = r0; r < r0 + n; r++)
			g->seen[gids[r]] += seen[r];
		for (int i = 0; i < s->naggs; i++)
			merge_agg(g, i, &ba[i], gids, r0, n);
	}
	for (int i = 0; i < s->naggs; i++)
		if (s->agg[i].kind == SAA_COUNT_DISTINCT)
			merge_set(g, i, &ba[i], gids);
		else if (is_ordered_agg(&s->agg[i]) && s->agg[i].share == i)
			merge_osa(g, i, &ba[i], gids);
	pfree(gids);
	pfree(ba);
}

/* ---------- the scan: items of work, lanes ---------- */

/*
 * The superblocks are split in items that the participants take from a counter. Float sums are order dependent, so that a result
 * does not depend on the number of workers: such a scan has SIFT_AGG_LANES items, lane l being the superblocks l, l + SIFT_AGG_LANES, ..., and the
 * tables of the lanes are combined in lane order, whoever scanned them. The other aggregates are exact in any order: items are
 * MORSEL_SBS superblocks, and a participant keeps one table for all its items.
 */
#define MORSEL_SBS	2

static Size
shared_size(const SiftAggSpec *s)
{
	return offsetof(ScanShared, partoff) + (s->nparts + 1) * sizeof(uint32);
}

static void
shared_init(ScanShared *sh, AggExec *ex)
{
	sh->nsb = sift_colsrc_nsb(ex->src);
	if (ex->spec->nparts > 0)
		sift_aggpart_offsets(ex->src, sh->partoff);
	sh->ordered = sift_aggspec_ordered(ex->spec);
	sh->morsel = sift_aggspec_parallel_ok(ex->spec) ? MORSEL_SBS : Max(sh->nsb, 1);
	sh->nitems = sh->ordered ? SIFT_AGG_LANES : (sh->nsb + sh->morsel - 1) / sh->morsel;
	pg_atomic_init_u32(&sh->next, 0);
}

static void
scan_units(AggExec *ex, uint32 lo, uint32 hi)
{
	lo = Max(lo, ex->tid_sb_lo);
	hi = Min(hi, ex->tid_sb_hi);
	if (lo >= hi)
		return;
	sift_colsrc_begin(ex->src, lo, hi, ex->prune, ex->nprune);
	while (sift_colsrc_next(ex->src, &ex->unit))
	{
		process_unit(ex);
		if (sift_aggbe_stopped(ex->be, ex->be_state))
			break;				/* the scan is run again without the backend (backend_finish) */
	}
}

/* With a match set, the source keeps the matching rows of the units of the range only (agg/search.h) */
static void
scan_range(AggExec *ex, uint32 lo, uint32 hi)
{
	if (ex->search == NULL)
	{
		scan_units(ex, lo, hi);
		return;
	}
	for (uint32 a = lo; a < hi; a += sift_search_chunk_sbs)
	{
		sift_colsrc_select(ex->src, sift_search_range(ex->search, a, Min(a + sift_search_chunk_sbs, hi)));
		scan_units(ex, a, Min(a + sift_search_chunk_sbs, hi));
	}
}

/* a grouping set without columns has its group even when no row reached it, as the plain aggregate has its row */
static void
empty_set_groups(AggExec *ex)
{
	for (int set = 0; set < ex->spec->nsets; set++)
		if (ex->spec->setmask[set] == (uint32) 1 << (ex->spec->nkeys - 1))
		{
			ex->curset = set;
			ex->arm_mask = ex->spec->setmask[set];
			compute_gids(ex, 1);
			ex->seen[ex->gid[0]] = Max(ex->seen[ex->gid[0]], 1);
		}
	ex->arm_mask = ~(uint32) 0;
}

static void
scan_item(AggExec *ex, uint32 item)
{
	ScanShared *sh = ex->shared;

	if (sh->ordered)
		for (uint32 sb = item; sb < sh->nsb; sb += SIFT_AGG_LANES)
			scan_range(ex, sb, sb + 1);
	else
		scan_range(ex, item * sh->morsel, Min((item + 1) * sh->morsel, sh->nsb));
}

static bool
take_item(AggExec *ex, uint32 *item)
{
	*item = pg_atomic_fetch_add_u32(&ex->shared->next, 1);
	return *item < ex->shared->nitems;
}

/* the lanes that are next in order go into the result */
static void
lanes_combine(AggExec *ex)
{
	while (ex->next_lane < SIFT_AGG_LANES && ex->pending[ex->next_lane])
	{
		blob_merge(ex->gtab, ex->pending[ex->next_lane]);
		pfree(ex->pending[ex->next_lane]);
		ex->pending[ex->next_lane++] = NULL;
	}
}

static void
lanes_put(AggExec *ex, int lane, const char *data, Size len)
{
	ex->pending[lane] = MemoryContextAlloc(ex->cxt, len);
	memcpy(ex->pending[lane], data, len);
	lanes_combine(ex);
}

static AggExec *
result_view(AggExec *ex)
{
	AggExec    *g = MemoryContextAlloc(ex->cxt, sizeof(AggExec));

	*g = *ex;
	g->lanes = false;
	g->tcxt = AllocSetContextCreate(ex->cxt, "sift agg result", ALLOCSET_DEFAULT_SIZES);
	g->ks = MemoryContextAlloc(ex->cxt, Max(ex->spec->nkeys, 1) * sizeof(KeyState));
	memcpy(g->ks, ex->ks, Max(ex->spec->nkeys, 1) * sizeof(KeyState));
	table_init(g);
	return g;
}

/* the check that avg(float8) owes (float_guard): the inputs of the whole scan could have overflowed the sum of squares */
static void
final_checks(AggExec *g, int64 ntuples)
{
	for (int i = 0; i < g->spec->naggs; i++)
		if (g->spec->agg[i].kind == SAA_AVG_F64 && 8.0 * ntuples * g->amax[i] * g->amax[i] >= DBL_MAX)
			float_overflow_error();
}

/* ---------- workers ---------- */

/*
 * A scan whose plan asks for workers runs them under a parallel context of its own (not under a Gather: the partial tables are
 * not tuples). The leader and the workers take items from the counter in the shared memory; a worker sends its tables to the
 * leader through a shm_mq, as blobs, and the leader merges them. Lanes wait for the lanes before them.
 */
#define KEY_SHARED	UINT64CONST(0x53494654A001)
#define KEY_SPEC	UINT64CONST(0x53494654A002)
#define KEY_PARAMS	UINT64CONST(0x53494654A003)
#define KEY_SNAP	UINT64CONST(0x53494654A004)
#define KEY_MQ		UINT64CONST(0x53494654A005)
#define MQ_SIZE		(1 << 20)
#define DONE_MAGIC	0x53414732

typedef struct WorkerDone
{
	uint32		magic;
	uint32		pad;
	int64		n_units;
	int64		n_pre_units;
	int64		n_rollup_miss;
	int64		n_rollup_queued;
	int64		n_rows;
	int64		n_tuples;
	int64		n_dense;
	int64		n_matches;
	SiftSrcCounters cc;
} WorkerDone;

static void
send_blob(shm_mq_handle *mqh, const void *data, Size len)
{
	if (shm_mq_send(mqh, len, data, false, true) != SHM_MQ_SUCCESS)
		ereport(ERROR, (errmsg("sift: the leader of a parallel aggregate went away")));
}

/* the table of the lane just scanned, as a blob: kept in order here, sent by a worker */
static void
lane_done(AggExec *ex, uint32 lane)
{
	StringInfoData b;

	initStringInfo(&b);
	blob_dump(ex, lane, &b);
	if (ex->mqh)
		send_blob(ex->mqh, b.data, b.len);
	else
		lanes_put(ex, lane, b.data, b.len);
	pfree(b.data);
	table_reset(ex);
}

/* the float sums of the lane just scanned are added to those of the lanes before it, as the leader adds the table of each lane */
static void
lane_fold(AggExec *ex)
{
	uint32		n = ex->hash ? ex->ngroups : ex->gcap;

	for (int i = 0; i < ex->spec->naggs; i++)
	{
		AggAcc	   *a = &ex->acc[i];

		if (a->dt)
			for (uint32 g = 0; g < n; g++)
			{
				a->dt[g] += a->d[g];
				a->d[g] = 0;
			}
	}
}

static void par_poll(AggExec *ex);
static void engine_contexts(AggExec *ex, MemoryContext parent);
static void engine_rels(AggExec *ex);
static void engine_open(AggExec *ex, Snapshot snapshot, ParamListInfo params);
static void engine_close(AggExec *ex);

void
scan_items(AggExec *ex)
{
	uint32		item;

	while (!sift_aggbe_stopped(ex->be, ex->be_state) && take_item(ex, &item))
	{
		scan_item(ex, item);
		if (ex->lane_serial)
			lane_fold(ex);
		else if (ex->shared->ordered)
			lane_done(ex, item);
		if (ex->par)
			par_poll(ex);
	}
}

void
sift_agg_worker_main(dsm_segment *seg, shm_toc *toc)
{
	ScanShared *sh = shm_toc_lookup(toc, KEY_SHARED, false);
	char	   *params = shm_toc_lookup(toc, KEY_PARAMS, false);
	shm_mq	   *mq = (shm_mq *) ((char *) shm_toc_lookup(toc, KEY_MQ, false) + (Size) ParallelWorkerNumber * MQ_SIZE);
	AggExec    *ex = palloc0(sizeof(AggExec));
	Snapshot	snap = RegisterSnapshot(RestoreSnapshot(shm_toc_lookup(toc, KEY_SNAP, false)));
	WorkerDone	done = {DONE_MAGIC};

	shm_mq_set_sender(mq, MyProc);
	ex->mqh = shm_mq_attach(mq, seg, NULL);
	ex->spec = sift_aggspec_unpack((List *) stringToNode((char *) shm_toc_lookup(toc, KEY_SPEC, false)));
	engine_contexts(ex, CurrentMemoryContext);
	engine_rels(ex);
	ex->snap = snap;
	engine_open(ex, snap, RestoreParamList(&params));
	if (ex->spec->nparts > 0)
		sift_aggpart_set_offsets(ex->src, sh->partoff);
	ex->shared = sh;
	ex->lanes = sh->ordered;
	scan_buffers(ex);
	table_init(ex);
	scan_items(ex);
	if (!sh->ordered)
	{
		StringInfoData b;

		initStringInfo(&b);
		blob_dump(ex, -1, &b);
		send_blob(ex->mqh, b.data, b.len);
	}
	done.n_units = ex->n_units;
	done.n_pre_units = ex->n_pre_units;
	done.n_rollup_miss = ex->n_rollup_miss;
	done.n_rollup_queued = ex->n_rollup_queued;
	done.n_rows = ex->n_rows;
	done.n_tuples = ex->n_tuples;
	done.n_dense = ex->n_dense;
	done.n_matches = ex->search ? sift_search_matches(ex->search) : 0;
	sift_colsrc_counters(ex->src, &done.cc);
	send_blob(ex->mqh, &done, sizeof(done));
	shm_mq_detach(ex->mqh);
	engine_close(ex);
	UnregisterSnapshot(snap);
}

/* Starts the workers of the plan; the leader scans alone when none could be launched */
static void
par_launch(AggExec *ex)
{
	ParallelContext *pcxt;
	char	   *spec_txt = nodeToString(ex->pack),
			   *p;
	Size		plen = EstimateParamListSpace(ex->params),
				slen = EstimateSnapshotSpace(ex->snap);
	int			nw = ex->spec->nworkers;
	ScanShared  *ps;
	AggPar	   *par;

	if (nw <= 0 || IsParallelWorker())
		return;
	EnterParallelMode();
	pcxt = CreateParallelContext("pg_sift", "sift_agg_worker_main", nw);
	shm_toc_estimate_chunk(&pcxt->estimator, shared_size(ex->spec));
	shm_toc_estimate_chunk(&pcxt->estimator, strlen(spec_txt) + 1);
	shm_toc_estimate_chunk(&pcxt->estimator, plen);
	shm_toc_estimate_chunk(&pcxt->estimator, slen);
	shm_toc_estimate_chunk(&pcxt->estimator, mul_size(MQ_SIZE, nw));
	shm_toc_estimate_keys(&pcxt->estimator, 5);
	InitializeParallelDSM(pcxt);
	if (pcxt->seg == NULL)
	{
		DestroyParallelContext(pcxt);
		ExitParallelMode();
		return;
	}
	ps = shm_toc_allocate(pcxt->toc, shared_size(ex->spec));
	shared_init(ps, ex);
	shm_toc_insert(pcxt->toc, KEY_SHARED, ps);
	p = shm_toc_allocate(pcxt->toc, strlen(spec_txt) + 1);
	strcpy(p, spec_txt);
	shm_toc_insert(pcxt->toc, KEY_SPEC, p);
	p = shm_toc_allocate(pcxt->toc, plen);
	shm_toc_insert(pcxt->toc, KEY_PARAMS, p);
	SerializeParamList(ex->params, &p);
	p = shm_toc_allocate(pcxt->toc, slen);
	SerializeSnapshot(ex->snap, p);
	shm_toc_insert(pcxt->toc, KEY_SNAP, p);
	p = shm_toc_allocate(pcxt->toc, mul_size(MQ_SIZE, nw));
	for (int i = 0; i < nw; i++)
		shm_mq_set_receiver(shm_mq_create(p + (Size) i * MQ_SIZE, MQ_SIZE), MyProc);
	shm_toc_insert(pcxt->toc, KEY_MQ, p);
	LaunchParallelWorkers(pcxt);
	if (pcxt->nworkers_launched == 0)
	{
		DestroyParallelContext(pcxt);
		ExitParallelMode();
		return;
	}
	par = MemoryContextAllocZero(ex->cxt, sizeof(AggPar));
	par->pcxt = pcxt;
	par->nlaunched = pcxt->nworkers_launched;
	par->mqh = MemoryContextAllocZero(ex->cxt, par->nlaunched * sizeof(shm_mq_handle *));
	par->done = MemoryContextAllocZero(ex->cxt, par->nlaunched * sizeof(bool));
	for (int i = 0; i < par->nlaunched; i++)
	{
		par->mqh[i] = shm_mq_attach((shm_mq *) (p + (Size) i * MQ_SIZE), pcxt->seg, NULL);
		shm_mq_set_handle(par->mqh[i], pcxt->worker[i].bgwhandle);
	}
	ex->par = par;
	ex->shared = ps;
}

static void
par_message(AggExec *ex, int w, const char *data, Size len)
{
	AggPar	   *par = ex->par;

	if (*(const uint32 *) data == DONE_MAGIC)
	{
		const WorkerDone *d = (const WorkerDone *) data;

		ex->n_units += d->n_units;
		ex->n_pre_units += d->n_pre_units;
		ex->n_rollup_miss += d->n_rollup_miss;
		ex->n_rollup_queued += d->n_rollup_queued;
		ex->n_rows += d->n_rows;
		ex->n_tuples += d->n_tuples;
		ex->n_dense += d->n_dense;
		par->matches += d->n_matches;
		par->cc.units_pruned += d->cc.units_pruned;
		par->cc.rows_vetted += d->cc.rows_vetted;
		par->cc.heap_pages += d->cc.heap_pages;
		par->cc.rechecked += d->cc.rechecked;
		par->done[w] = true;
		par->ndone++;
		shm_mq_detach(par->mqh[w]);
	}
	else if (ex->shared->ordered)
	{
		const BlobHdr *h = (const BlobHdr *) data;

		if (h->magic != BLOB_MAGIC)
			elog(ERROR, "sift: bad message of a parallel aggregate");
		lanes_put(ex, h->lane, data, len);
	}
	else
		blob_merge(ex->gtab, data);
}

/* the messages that are there; whether there were any */
static bool
par_receive(AggExec *ex)
{
	AggPar	   *par = ex->par;
	bool		any = false;

	for (int w = 0; w < par->nlaunched; w++)
		while (!par->done[w])
		{
			Size		len;
			void	   *data;
			shm_mq_result r = shm_mq_receive(par->mqh[w], &len, &data, true);

			if (r == SHM_MQ_WOULD_BLOCK)
				break;
			if (r == SHM_MQ_DETACHED)
				ereport(ERROR, (errmsg("sift: a worker of a parallel aggregate stopped without its result")));
			par_message(ex, w, data, len);
			any = true;
		}
	return any;
}

static void
par_poll(AggExec *ex)
{
	par_receive(ex);
	CHECK_FOR_INTERRUPTS();
}

/* waits for the workers' tables, then for the workers */
static void
par_finish(AggExec *ex)
{
	AggPar	   *par = ex->par;

	while (par->ndone < par->nlaunched)
	{
		if (!par_receive(ex))
		{
			(void) WaitLatch(MyLatch, WL_LATCH_SET | WL_EXIT_ON_PM_DEATH, 0, PG_WAIT_EXTENSION);
			ResetLatch(MyLatch);
		}
		CHECK_FOR_INTERRUPTS();
	}
	WaitForParallelWorkersToFinish(par->pcxt);
	DestroyParallelContext(par->pcxt);
	par->pcxt = NULL;
	ExitParallelMode();
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

static void
engine_contexts(AggExec *ex, MemoryContext parent)
{
	ex->cxt = AllocSetContextCreate(parent, "sift agg", ALLOCSET_DEFAULT_SIZES);
	ex->tcxt = AllocSetContextCreate(ex->cxt, "sift agg table", ALLOCSET_DEFAULT_SIZES);
	ex->ucxt = AllocSetContextCreate(ex->cxt, "sift agg unit", ALLOCSET_DEFAULT_SIZES);
}

static void
engine_rels(AggExec *ex)
{
	if (OidIsValid(ex->spec->indexoid))
		ex->index = index_open(ex->spec->indexoid, AccessShareLock);
	ex->heap = table_open(ex->spec->heapoid, AccessShareLock);
	ex->heap_opened = true;
}

/* The scan engine on the relations of the spec: shared by the node and by a parallel worker */
static void
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

static void
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
