/*
 * agg/sip_scan.c - the path and the node of SiftAggV6Sip, and the wrapper that applies a filter to the batches of a source.
 */
#include "postgres.h"

#include "access/genam.h"
#include "access/relation.h"
#include "access/table.h"
#include "access/sysattr.h"
#include "access/tableam.h"
#include "catalog/pg_namespace_d.h"
#include "catalog/pg_type_d.h"
#include "executor/executor.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "optimizer/clauses.h"
#include "optimizer/cost.h"
#include "optimizer/optimizer.h"
#include "optimizer/pathnode.h"
#include "optimizer/restrictinfo.h"
#include "parser/parsetree.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"
#include "utils/selfuncs.h"

#include "agg/agg.h"
#include "agg/safety.h"
#include "agg/source_rel.h"
#include "agg/cost/card.h"
#include "agg/sip_scan.h"
#include "agg/settings.h"
#include "core/memtrack.h"
#include "core/compat.h"

SiftSipFilter *
sift_v6_sip_start(MemoryContext cxt, SiftVecType key_type, double est_ndv)
{
	return sift_sip_create(cxt, key_type, (uint64) Max(est_ndv, 1.0), sift_sip_budget_bytes());
}

void
sift_v6_sip_add_key(SiftSipFilter *sip, int64 key)
{
	sift_sip_insert_i64(sip, key);
}

void
sift_v6_sip_finish(SiftSipFilter *sip)
{
	sift_sip_finalize(sip);
}

typedef struct SipSource
{
	SiftV6Source base;
	SiftV6Source *inner;
	const SiftSipFilter *sip;
	int			key_slot;
	SiftSipStats *stats;
	SiftSipProbeState ps;
	uint64		mask[SIFT_VEC_WORDS];
} SipSource;

static bool
wrap_next(SiftV6Source *s, SiftV6Batch *out)
{
	SipSource  *w = (SipSource *) s;

	while (w->inner->next(w->inner, out))
	{
		const int	words = (out->n + 63) / 64;

		memset(w->mask, 0, sizeof(w->mask));
		if (out->mask != NULL)
			memcpy(w->mask, out->mask, words * sizeof(uint64));
		else
			for (int i = 0; i < out->n; i++)
				w->mask[i / 64] |= UINT64_C(1) << (i % 64);
		if (sift_sip_probe_vec_adapt(w->sip, &w->ps, &out->col[w->key_slot], w->mask) == 0)
			continue;
		out->mask = w->mask;
		return true;
	}
	return false;
}

static void
wrap_skip(SiftV6Source *s, const int64 *part_key, uint8 part_nulls)
{
	SipSource  *w = (SipSource *) s;

	if (w->inner->skip != NULL)
		w->inner->skip(w->inner, part_key, part_nulls);
}

static bool
wrap_fetch(SiftV6Source *s, uint64 tag, const AttrNumber *attnos, int n, Datum *values, bool *isnull)
{
	SipSource  *w = (SipSource *) s;

	return w->inner->fetch(w->inner, tag, attnos, n, values, isnull);
}

static void
wrap_close(SiftV6Source *s)
{
	SipSource  *w = (SipSource *) s;

	if (w->stats != NULL)
		*w->stats = w->ps.stats;
	w->inner->close(w->inner);
	pfree(w);
}

SiftV6Source *
sift_v6_sip_wrap(MemoryContext cxt, SiftV6Source *inner, const SiftSipFilter *sip, int key_slot, SiftSipStats *stats)
{
	SipSource  *w = MemoryContextAllocZero(cxt, sizeof(SipSource));

	w->base = (SiftV6Source) {.next = wrap_next,.skip = wrap_skip,.fetch = wrap_fetch,.close = wrap_close};
	w->inner = inner;
	w->sip = sip;
	w->key_slot = key_slot;
	w->stats = stats;
	return &w->base;
}

/* ---- the planner ---- */

enum
{
	PRIV_INTS = 0,				/* source oid, fact rti, dimension relid, dimension rti, fact key attno, dimension key attno, estimated dimension rows, the source is a sift_table */
	PRIV_FACT_QUALS = 1,		/* the conditions of the fact table the scan can test on the stored values */
	PRIV_DIM_QUALS = 2
};

typedef struct SipJoin
{
	Var		   *fact_key;
	Var		   *dim_key;
	RelOptInfo *dim;
	double		kept;			/* the share of the rows of the fact table whose key a row of the dimension that passes its conditions has */
} SipJoin;

static bool
same_domain(Oid a, Oid b)
{
	const bool	ints = (a == INT2OID || a == INT4OID || a == INT8OID) && (b == INT2OID || b == INT4OID || b == INT8OID);

	return ints || (a == b && (a == DATEOID || a == TIMESTAMPOID || a == TIMESTAMPTZOID));
}

/* a join of the fact table a with the dimension b that drops every row of a without a match: neither is on the side of a join that may null it or a full join's, but b in a semi join of a.
 * A clause the planner did not push down is a semi join's own (b is in its right side) or one an outer join tests without dropping a row, which no filter may use */
static bool
filtering_join(PlannerInfo *root, Index a, Index b, bool pushed)
{
	bool		semi = false;

	foreach_node(SpecialJoinInfo, sj, root->join_info_list)
	{
		const bool	a_in = bms_is_member(a, sj->syn_righthand),
					b_in = bms_is_member(b, sj->syn_righthand);

		if (sj->jointype == JOIN_SEMI && b_in && bms_is_member(a, sj->syn_lefthand))
			semi = true;
		else if (a_in || b_in || (sj->jointype == JOIN_FULL && (bms_is_member(a, sj->syn_lefthand) || bms_is_member(b, sj->syn_lefthand))))
			return false;
	}
	return pushed || semi;
}

static bool
join_of(PlannerInfo *root, RelOptInfo *rel, const Node *l, const Node *r, bool pushed, SipJoin *j)
{
	const RangeTblEntry *rte;

	if (!IsA(l, Var) || !IsA(r, Var) || ((Var *) l)->varno != rel->relid || ((Var *) r)->varno == rel->relid || ((Var *) l)->varlevelsup != 0 ||
		((Var *) r)->varlevelsup != 0 || ((Var *) l)->varnullingrels != NULL || ((Var *) r)->varnullingrels != NULL || !same_domain(((Var *) l)->vartype, ((Var *) r)->vartype))
		return false;
	j->dim = root->simple_rel_array[((Var *) r)->varno];
	if (j->dim == NULL || j->dim->reloptkind != RELOPT_BASEREL || !filtering_join(root, rel->relid, j->dim->relid, pushed))
		return false;
	rte = planner_rt_fetch(j->dim->relid, root);
	if (!sift_v6_rte_stored(rte) || rte->inh || rte->tablesample != NULL || agg_rte_has_rls(root, rte) || j->dim->rows > SIFT_V6_SIP_DIM_ROWS)
		return false;
	foreach_node(RestrictInfo, ri, j->dim->baserestrictinfo)
		if (ri->pseudoconstant || contain_volatile_functions((Node *) ri->clause) || contain_subplans((Node *) ri->clause))
			return false;
	j->fact_key = (Var *) l;
	j->dim_key = (Var *) r;
	return true;
}

static List *
clauses_of(const List *restrictinfos)
{
	List	   *l = NIL;

	foreach_node(RestrictInfo, ri, (List *) restrictinfos)
		l = lappend(l, ri->clause);
	return l;
}

/* the conditions of the fact table the scan tests on its columns: those the machine compiles, each alone (PostgreSQL tests all of them again on what the scan gives) */
static List *
pushable(RelOptInfo *rel, const SiftV6Input *source)
{
	List	   *pushed = NIL;

	foreach_node(RestrictInfo, ri, rel->baserestrictinfo)
	{
		SiftV6Input in = *source;
		SiftV6Vexpr probe;

		if (!ri->pseudoconstant && sift_v6_vexpr_quals(CurrentMemoryContext, list_make1(ri->clause), rel->relid, NULL, &in, &probe))
			pushed = lappend(pushed, ri->clause);
	}
	return pushed;
}

static const CustomPathMethods sip_path_methods = {"SiftAggV6Sip", sift_agg_v6_sip_plan_path, NULL};

/* the scan gives the columns of the table, not its system columns (ctid): a condition or output that reads one is the heap's */
static bool
reads_system_column(RelOptInfo *rel)
{
	Bitmapset  *attrs = NULL;
	int			first;

	foreach_node(RestrictInfo, ri, rel->baserestrictinfo)
		pull_varattnos((Node *) ri->clause, rel->relid, &attrs);
	pull_varattnos((Node *) rel->reltarget->exprs, rel->relid, &attrs);
	first = bms_next_member(attrs, -1);
	return first >= 0 && first < -FirstLowInvalidHeapAttributeNumber;
}

static Node *
to_fact_key(Node *node, const SipJoin *j)
{
	if (equal(node, j->dim_key))
		return copyObject((Node *) j->fact_key);
	return expression_tree_mutator(node, to_fact_key, (void *) j);
}

/* the share of the rows of the fact table the keys of the dimension keep: a condition of the dimension on its key alone is one on the key of the fact table, which the statistics of that column
 * price as they are spread (a few keys may hold most rows); the others keep a key unless they drop each of its rows */
static double
kept_share(PlannerInfo *root, RelOptInfo *rel, const SipJoin *j)
{
	const bool	same_type = j->fact_key->vartype == j->dim_key->vartype && j->fact_key->varcollid == j->dim_key->varcollid;
	VariableStatData vd;
	bool		isdefault;
	List	   *others = NIL;
	double		per_key,
				kept = 1.0;

	examine_variable(root, (Node *) j->dim_key, 0, &vd);
	per_key = Max(j->dim->tuples / Max(get_variable_numdistinct(&vd, &isdefault), 1.0), 1.0);
	ReleaseVariableStats(vd);
	foreach_node(RestrictInfo, ri, j->dim->baserestrictinfo)
	{
		Bitmapset  *attrs = NULL;

		pull_varattnos((Node *) ri->clause, j->dim->relid, &attrs);
		if (bms_membership(attrs) == BMS_SINGLETON && bms_is_member(j->dim_key->varattno - FirstLowInvalidHeapAttributeNumber, attrs))
			kept *= same_type ? clause_selectivity(root, to_fact_key((Node *) ri->clause, j), rel->relid, JOIN_INNER, NULL) : clause_selectivity(root, (Node *) ri, 0, JOIN_INNER, NULL);
		else
			others = lappend(others, ri);
	}
	return kept * -expm1(per_key * log1p(-clauselist_selectivity(root, others, j->dim->relid, JOIN_INNER, NULL)));
}

static bool
sip_path(PlannerInfo *root, RelOptInfo *rel, const SiftV6RelSource *src, const SipJoin *j)
{
	SiftV6Input in = src->input;
	const int	slot = sift_v6_input_add_var(&in, j->fact_key);
	const double rows = clamp_row_est(rel->rows * j->kept);
	Cost		open,
				scan;
	List	   *ints;

	if (slot < 0 || (in.col[slot].type != SVEC_I32 && in.col[slot].type != SVEC_I64) || reads_system_column(rel))
		return false;
	ints = list_make5_int((int) src->reloid, (int) rel->relid, (int) planner_rt_fetch(j->dim->relid, root)->relid, (int) j->dim->relid, j->fact_key->varattno);
	ints = lappend_int(lappend_int(ints, j->dim_key->varattno), (int) Min(j->dim->rows, (double) INT_MAX));
	ints = lappend_int(ints, src->is_table);
	sift_v6_source_scan_price(root, rel, src, j->fact_key->varattno, &open, &scan);
	sift_v6_add_path(rel, &sip_path_methods, open, scan + sift_v6_fetch_cost(src->is_table) * rows + sift_v6_rel_read_cost(j->dim) + cpu_operator_cost * rel->rows, rows, NIL,
					 list_make3(ints, pushable(rel, &src->input), clauses_of(j->dim->baserestrictinfo)), NIL);
	return true;
}

static bool
sip_join(PlannerInfo *root, RelOptInfo *rel, const Node *fact_key, const Node *dim_key, bool pushed)
{
	SipJoin		j;
	SiftV6RelSource *src;
	int			nsrc;

	if (!join_of(root, rel, fact_key, dim_key, pushed, &j))
		return false;
	j.kept = kept_share(root, rel, &j);
	if (j.kept > SIFT_V6_SIP_DIM_KEEP)
		return false;
	src = sift_v6_rel_sources_alloc(root, rel, &nsrc);
	for (int i = 0; i < nsrc; i++)
		if (sift_v6_source_usable(&src[i]) && sip_path(root, rel, &src[i], &j))
			return true;
	return false;
}

/* an equality of the fact table with another table is a clause of the join or a pair of members of one equivalence class */
static bool
sip_joins(PlannerInfo *root, RelOptInfo *rel)
{
	foreach_node(RestrictInfo, ri, rel->joininfo)
	{
		const OpExpr *op = (OpExpr *) ri->clause;
		const bool	fact_left = bms_is_member(rel->relid, ri->left_relids);

		if (ri->mergeopfamilies != NIL && IsA(ri->clause, OpExpr) && list_length(op->args) == 2 && bms_membership(ri->left_relids) == BMS_SINGLETON &&
			bms_membership(ri->right_relids) == BMS_SINGLETON && sip_join(root, rel, fact_left ? linitial(op->args) : lsecond(op->args), fact_left ? lsecond(op->args) : linitial(op->args), ri->is_pushed_down))
			return true;
	}
	foreach_node(EquivalenceClass, ec, root->eq_classes)
	{
		if (ec->ec_merged != NULL || ec->ec_has_const || ec->ec_has_volatile || !bms_is_member(rel->relid, ec->ec_relids))
			continue;
		foreach_node(EquivalenceMember, fact, ec->ec_members)
		{
			if (fact->em_is_child)
				continue;
			foreach_node(EquivalenceMember, dim, ec->ec_members)
				if (!dim->em_is_child && sip_join(root, rel, (Node *) fact->em_expr, (Node *) dim->em_expr, true))
					return true;
		}
	}
	return false;
}

void
sift_agg_v6_sip_rel_paths(PlannerInfo *root, RelOptInfo *rel, Index rti, RangeTblEntry *rte)
{
	if (!sift_enable_agg || rte->rtekind != RTE_RELATION || rel->reloptkind != RELOPT_BASEREL || !sift_v6_base_rel_ok(root, rel))
		return;
	(void) sip_joins(root, rel);
}

Plan *
sift_agg_v6_sip_plan_path(PlannerInfo *root, RelOptInfo *rel, CustomPath *best, List *tlist, List *clauses, List *custom_plans)
{
	CustomScan *cs = makeNode(CustomScan);
	const List *ints = linitial(best->custom_private);

	cs->scan.plan.targetlist = tlist;
	cs->scan.plan.qual = extract_actual_clauses(clauses, false);
	cs->scan.scanrelid = rel->relid;
	cs->flags = best->flags;
	cs->custom_private = best->custom_private;
	sift_v6_collect_params((Node *) list_nth(best->custom_private, PRIV_FACT_QUALS), &cs->custom_exprs);
	sift_v6_collect_params((Node *) list_nth(best->custom_private, PRIV_DIM_QUALS), &cs->custom_exprs);
	cs->methods = &sift_agg_v6_sip_plan_methods;
	root->glob->relationOids = lappend_oid(lappend_oid(root->glob->relationOids, (Oid) list_nth_int(ints, 0)), (Oid) list_nth_int(ints, 2));
	return &cs->scan.plan;
}

/* ---- the node ---- */

typedef struct SipState
{
	CustomScanState css;
	List	   *priv;
	ExprState  *dimqual;
	AttrNumber *attnos;			/* the columns of the table the plan reads */
	int			nattrs;
	Datum	   *fvals;
	bool	   *fnull;
	MemoryContext cx;
	SiftSipFilter *sip;
	SiftV6Source *src;
	SiftSipStats stats;
	SiftV6Batch b;
	uint16		pos;
	bool		in_batch;
	bool		ran;
} SipState;

static void
sip_begin(CustomScanState *node, EState *estate, int eflags)
{
	SipState   *st = (SipState *) node;
	const CustomScan *cs = (CustomScan *) node->ss.ps.plan;
	Relation	rel = node->ss.ss_currentRelation;
	Bitmapset  *attrs = NULL;
	int			natts = RelationGetNumberOfAttributes(rel);

	st->priv = cs->custom_private;
	st->dimqual = ExecInitQual(list_nth(st->priv, PRIV_DIM_QUALS), NULL);
	pull_varattnos((Node *) cs->scan.plan.targetlist, cs->scan.scanrelid, &attrs);
	pull_varattnos((Node *) cs->scan.plan.qual, cs->scan.scanrelid, &attrs);
	st->attnos = palloc(natts * sizeof(AttrNumber));
	for (AttrNumber a = 1; a <= natts; a++)
		if (!TupleDescAttr(RelationGetDescr(rel), a - 1)->attisdropped &&
			(bms_is_member(0 - FirstLowInvalidHeapAttributeNumber, attrs) || bms_is_member(a - FirstLowInvalidHeapAttributeNumber, attrs)))
			st->attnos[st->nattrs++] = a;
	st->fvals = palloc(Max(st->nattrs, 1) * sizeof(Datum));
	st->fnull = palloc(Max(st->nattrs, 1) * sizeof(bool));
}

/* the keys of the rows of the dimension that pass its conditions */
static SiftSipFilter *
sip_build(SipState *st, EState *estate, SiftVecType key_type)
{
	const List *ints = linitial(st->priv);
	Relation	dim = table_open((Oid) list_nth_int(ints, 2), AccessShareLock);
	ExprContext *ec = st->css.ss.ps.ps_ExprContext;
	TableScanDesc scan = sift_table_beginscan(dim, estate->es_snapshot, 0, NULL);
	TupleTableSlot *slot = table_slot_create(dim, NULL);
	SiftSipFilter *sip = sift_v6_sip_start(st->cx, key_type, (double) list_nth_int(ints, 6));
	const AttrNumber key = (AttrNumber) list_nth_int(ints, 5);
	const Oid	typid = TupleDescAttr(RelationGetDescr(dim), key - 1)->atttypid;

	while (table_scan_getnextslot(scan, ForwardScanDirection, slot))
	{
		bool		isnull;
		Datum		d;

		ec->ecxt_scantuple = slot;
		if (st->dimqual != NULL && !ExecQual(st->dimqual, ec))
		{
			ResetExprContext(ec);
			continue;
		}
		d = slot_getattr(slot, key, &isnull);
		if (!isnull)
			sift_v6_sip_add_key(sip, typid == INT2OID ? DatumGetInt16(d) : typid == INT8OID || typid == TIMESTAMPOID || typid == TIMESTAMPTZOID ? DatumGetInt64(d) : DatumGetInt32(d));
		ResetExprContext(ec);
		CHECK_FOR_INTERRUPTS();
	}
	ExecDropSingleTupleTableSlot(slot);
	table_endscan(scan);
	table_close(dim, NoLock);
	sift_v6_sip_finish(sip);
	return sip;
}

static void
sip_run(SipState *st, EState *estate)
{
	const List *ints = linitial(st->priv);
	const List *pushed = list_nth(st->priv, PRIV_FACT_QUALS);
	SiftV6Input in;
	SiftV6Vexpr quals;
	const bool	is_table = list_nth_int(ints, 7);
	Relation	source = sift_v6_source_open((Oid) list_nth_int(ints, 0), is_table, AccessShareLock, &in);
	const AttrNumber fact_key = (AttrNumber) list_nth_int(ints, 4);
	Var		   *key;
	int			slot;
	bool		compiled;

	sift_v6_source_enter(source, is_table, estate->es_snapshot);
	st->cx = sift_mem_allocset_create(estate->es_query_cxt, "sift agg v6 sip");
	sift_v6_input_bind(&in, estate);
	key = makeVar((Index) list_nth_int(ints, 1), fact_key, TupleDescAttr(RelationGetDescr(st->css.ss.ss_currentRelation), fact_key - 1)->atttypid, -1, InvalidOid, 0);
	sift_v6_source_close(source, is_table, NoLock);
	compiled = (slot = sift_v6_input_add_var(&in, key)) >= 0 && sift_v6_vexpr_quals(st->cx, pushed, (Index) list_nth_int(ints, 1), estate->es_param_list_info, &in, &quals);
	SIFT_INVARIANT(compiled, "the filtered scan of relation %u is not compiled", (Oid) list_nth_int(ints, 0));
	st->sip = sip_build(st, estate, in.col[slot].type);
	st->ran = true;
	if (sift_sip_is_empty(st->sip))
		return;
	st->src = sift_v6_sip_wrap(st->cx, sift_v6_rel_open(estate, &in, pushed == NIL ? NULL : &quals, false), st->sip, slot, &st->stats);
}

static TupleTableSlot *
sip_next(ScanState *ss)
{
	SipState   *st = (SipState *) ss;
	TupleTableSlot *slot = ss->ss_ScanTupleSlot;
	bool		fetched;

	if (!st->ran)
		sip_run(st, ss->ps.state);
	while (st->src != NULL)
	{
		if (!st->in_batch)
		{
			if (!st->src->next(st->src, &st->b))
				return NULL;
			st->in_batch = true;
			st->pos = 0;
		}
		while (st->pos < st->b.n)
		{
			const int	r = st->pos++;

			if (st->b.mask != NULL && !((st->b.mask[r / 64] >> (r % 64)) & 1))
				continue;
			ExecClearTuple(slot);
			memset(slot->tts_isnull, true, slot->tts_tupleDescriptor->natts * sizeof(bool));
			fetched = st->b.tag != NULL && st->src->fetch(st->src, st->b.tag[r], st->attnos, st->nattrs, st->fvals, st->fnull);
			SIFT_INVARIANT(fetched, "a row of the filtered scan has no columns to fetch");
			for (int i = 0; i < st->nattrs; i++)
			{
				slot->tts_values[st->attnos[i] - 1] = st->fvals[i];
				slot->tts_isnull[st->attnos[i] - 1] = st->fnull[i];
			}
			return ExecStoreVirtualTuple(slot);
		}
		st->in_batch = false;
	}
	return NULL;
}

static bool
sip_recheck(ScanState *ss, TupleTableSlot *slot)
{
	return true;
}

static TupleTableSlot *
sip_exec(CustomScanState *node)
{
	return ExecScan(&node->ss, sip_next, sip_recheck);
}

static void
sip_release(SipState *st)
{
	if (st->src != NULL)
		st->src->close(st->src);
	if (st->cx != NULL)
		MemoryContextDelete(st->cx);
	st->src = NULL;
	st->sip = NULL;
	st->cx = NULL;
	st->in_batch = false;
	st->ran = false;
}

static void
sip_end(CustomScanState *node)
{
	sip_release((SipState *) node);
}

static void
sip_rescan(CustomScanState *node)
{
	sip_release((SipState *) node);
}

static void
sip_explain(CustomScanState *node, List *ancestors, ExplainState *es)
{
	SipState   *st = (SipState *) node;

	sift_v6_explain_index(es, (Oid) linitial_int(linitial(st->priv)), list_nth_int(linitial(st->priv), 7));
	ExplainPropertyList("Lookup Joins", list_make1(get_rel_name((Oid) list_nth_int(linitial(st->priv), 2))), es);
	if (st->ran)
	{
		sift_v6_explain_count(es, "Filter Rows In", (int64) st->stats.rows_in);
		sift_v6_explain_count(es, "Filter Rows Out", (int64) st->stats.rows_out);
	}
}

static const CustomExecMethods sip_exec_methods = {
	.CustomName = "SiftAggV6Sip",
	.BeginCustomScan = sip_begin,
	.ExecCustomScan = sip_exec,
	.EndCustomScan = sip_end,
	.ReScanCustomScan = sip_rescan,
	.ExplainCustomScan = sip_explain,
};

static Node *
sip_create_state(CustomScan *cscan)
{
	SipState   *st = (SipState *) newNode(sizeof(SipState), T_CustomScanState);

	st->css.methods = &sip_exec_methods;
	st->css.slotOps = &TTSOpsVirtual;
	return (Node *) st;
}

const CustomScanMethods sift_agg_v6_sip_plan_methods = {"SiftAggV6Sip", sip_create_state};
