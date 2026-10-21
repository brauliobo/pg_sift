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
