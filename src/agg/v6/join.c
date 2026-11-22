/*
 * agg/v6/join.c - the partitioned (Grace) hash join of SiftAggV6: the path of an inner, left, semi or anti join of columns compared for equality whose build side does not fit in half of work_mem, and
 * the node that runs it through v6_grace_join.h instead of the batches of PostgreSQL's hash join.
 *
 * The path takes the cheapest unparameterized paths of the two sides as its children (PostgreSQL plans them, with every scan and filter of its own), the outer side is probed and the inner one built. The
 * keys are plain columns of the children, which the node encodes as the exact bytes of v6_distinct_pg.h (the integer types are one key type, the float types another: two columns of other types are no
 * key); a join clause that is anything else is no join for this node and the path is not offered. Each child tuple is the payload of its row, a minimal tuple the node stores in the join's files and
 * reads back, so the join holds no more than its share of work_mem whatever the rows. The rows it makes go to a tuplestore (SiftV6Store) as the tuple of the scan the planner asked for: the columns of
 * the outer child and then those of the inner one, the inner ones NULL for a probe row of a left join that matched nothing.
 */
/* check-memory: allow-allocs every array here follows the columns of one statement, not the rows of the tables */
#include "postgres.h"

#include "access/htup_details.h"
#include "catalog/pg_type_d.h"
#include "commands/explain.h"
#include "executor/executor.h"
#include "miscadmin.h"
#include "nodes/extensible.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "optimizer/cost.h"
#include "optimizer/optimizer.h"
#include "optimizer/pathnode.h"
#include "optimizer/paths.h"
#include "parser/parsetree.h"

#include "agg/agg.h"
#include "agg/v6/v6_distinct_pg.h"
#include "agg/v6/v6_grace_join.h"
#include "agg/v6/v6_join.h"
#include "agg/v6/v6_plan_shape.h"
#include "agg/v6/v6_vexpr.h"

#define JOIN_MAX_KEYS	8

bool
sift_v6_rels_have_index(PlannerInfo *root, Relids relids)
{
	int			i = -1;

	if (!sift_v6_format_bound())
		return false;
	while ((i = bms_next_member(relids, i)) >= 0)
	{
		const RelOptInfo *r = root->simple_rel_array[i];
		const RangeTblEntry *rte = planner_rt_fetch(i, root);

		if (r == NULL || r->reloptkind != RELOPT_BASEREL)
			continue;
		if (rte->rtekind == RTE_RELATION && sift_v6_format()->owns_rel(rte->relid, true))
			return true;
		foreach_ptr(IndexOptInfo, ix, r->indexlist)
			if (!ix->hypothetical && sift_v6_format()->owns_rel(ix->indexoid, false))
				return true;
	}
	return false;
}

int
sift_v6_child_resno(const List *tlist, const Var *v)
{
	foreach_node(TargetEntry, te, (List *) tlist)
		if (IsA(te->expr, Var) && ((Var *) te->expr)->varno == v->varno && ((Var *) te->expr)->varattno == v->varattno)
			return te->resno;
	return 0;
}

bool
sift_v6_target_has(const PathTarget *target, const Var *v)
{
	foreach_ptr(Expr, e, target->exprs)
		if (IsA(e, Var) && ((Var *) e)->varno == v->varno && ((Var *) e)->varattno == v->varattno)
			return true;
	return false;
}

/* ---- the planner ---- */

static SiftShapeJoinType
shape_type(JoinType t)
{
	switch (t)
	{
		case JOIN_INNER:
			return SSJ_INNER;
		case JOIN_LEFT:
			return SSJ_LEFT;
		case JOIN_RIGHT:
			return SSJ_RIGHT;
		case JOIN_SEMI:
			return SSJ_SEMI;
		case JOIN_ANTI:
			return SSJ_ANTI;
		default:
			return SSJ_FULL;
	}
}

/* the key type two columns are compared as: the integer types are one, the float types another, and text and varchar one */
static Oid
key_family(Oid t)
{
	switch (t)
	{
		case INT2OID:
		case INT4OID:
			return INT8OID;
		case FLOAT4OID:
			return FLOAT8OID;
		case VARCHAROID:
			return TEXTOID;
		default:
			return t;
	}
}

static bool
key_pair_ok(Oid l, Oid r, Oid collation)
{
	return sift_distinct_type_ok(l) && sift_distinct_type_ok(r) && key_family(l) == key_family(r) && sift_distinct_collation_ok(collation);
}

/* the clauses of the join as the shape counts them; the key columns of each side in okeys and ikeys */
static void
classify(const RelOptInfo *outerrel, const RelOptInfo *innerrel, const List *restrictlist, SiftJoinShape *shape, List **okeys, List **ikeys)
{
	foreach_node(RestrictInfo, ri, (List *) restrictlist)
	{
		const OpExpr *op = (const OpExpr *) ri->clause;
		Node	   *l,
				   *r;

		if (!IsA(ri->clause, OpExpr) || list_length(op->args) != 2 || !OidIsValid(ri->hashjoinoperator))
		{
			shape->nother++;
			continue;
		}
		l = linitial(op->args);
		r = lsecond(op->args);
		if (bms_is_subset(ri->left_relids, innerrel->relids) && bms_is_subset(ri->right_relids, outerrel->relids))
		{
			Node	   *t = l;

			l = r;
			r = t;
		}
		else if (!bms_is_subset(ri->left_relids, outerrel->relids) || !bms_is_subset(ri->right_relids, innerrel->relids))
		{
			shape->nother++;
			continue;
		}
		if (!IsA(l, Var) || !IsA(r, Var) || !key_pair_ok(exprType(l), exprType(r), op->inputcollid) || list_length(*okeys) == JOIN_MAX_KEYS)
		{
			shape->nunhashable++;
			continue;
		}
		*okeys = lappend(*okeys, l);
		*ikeys = lappend(*ikeys, r);
		shape->nequi++;
	}
}

/* every column the join gives is a column of a child: the node copies columns, it does not compute any */
static bool
resolvable(const RelOptInfo *joinrel, const Path *op, const Path *ip, SiftJoinType type)
{
	foreach_ptr(Expr, e, joinrel->reltarget->exprs)
	{
		const Var  *v = (const Var *) e;

		if (!IsA(e, Var) || v->varlevelsup != 0)
			return false;
		if (!sift_v6_target_has(op->pathtarget, v) && (type == SGJ_SEMI || type == SGJ_ANTI || !sift_v6_target_has(ip->pathtarget, v)))
			return false;
	}
	return true;
}

static Plan *join_plan_path(PlannerInfo *root, RelOptInfo *rel, CustomPath *best, List *tlist, List *clauses, List *custom_plans);

static const CustomPathMethods join_path_methods = {"SiftAggV6Join", join_plan_path, NULL};

void
sift_agg_v6_join_paths(PlannerInfo *root, RelOptInfo *joinrel, RelOptInfo *outerrel, RelOptInfo *innerrel, JoinType jointype, JoinPathExtraData *extra)
{
	SiftJoinShape shape = {.type = shape_type(jointype),.work_mem_bytes = (uint64) work_mem * 1024};
	List	   *okeys = NIL,
			   *ikeys = NIL;
	const Path *op = outerrel->cheapest_total_path;
	const Path *ip = innerrel->cheapest_total_path;
	SiftJoinType type;
	CustomPath *cp;
	double		build_bytes,
				spilled,
				startup;

	if (!sift_enable_agg || root->parse->commandType != CMD_SELECT || root->rowMarks != NIL || outerrel->lateral_relids != NULL || innerrel->lateral_relids != NULL ||
		op == NULL || ip == NULL || op->param_info != NULL || ip->param_info != NULL || !sift_v6_rels_have_index(root, joinrel->relids))
		return;
	shape.build_rows = ip->rows;
	shape.build_width = ip->pathtarget->width;
	classify(outerrel, innerrel, extra->restrictlist, &shape, &okeys, &ikeys);
	if (sift_shape_join(&shape, &type) != SSV_OK || !resolvable(joinrel, op, ip, type))
		return;

	build_bytes = (double) sift_shape_build_bytes(ip->rows, ip->pathtarget->width);
	spilled = Max(0.0, 1.0 - (double) (shape.work_mem_bytes / 2) / Max(build_bytes, 1.0));
	startup = ip->total_cost + (cpu_tuple_cost + cpu_operator_cost * shape.nequi) * ip->rows + 2.0 * seq_page_cost * spilled * (build_bytes / BLCKSZ);
	cp = makeNode(CustomPath);
	cp->path.pathtype = T_CustomScan;
	cp->path.parent = joinrel;
	cp->path.pathtarget = joinrel->reltarget;
	cp->path.rows = joinrel->rows;
	cp->path.startup_cost = startup;
	cp->path.total_cost = startup + op->total_cost + (cpu_tuple_cost + cpu_operator_cost * shape.nequi) * op->rows + cpu_tuple_cost * joinrel->rows +
		2.0 * seq_page_cost * spilled * ((double) sift_shape_build_bytes(op->rows, op->pathtarget->width) / BLCKSZ);
	cp->custom_paths = list_make2((Path *) op, (Path *) ip);
	cp->custom_private = list_make3(list_make3_int((int) type, (int) (uint32) (uint64) build_bytes, (int) (uint32) ((uint64) build_bytes >> 32)), okeys, ikeys);
	cp->methods = &join_path_methods;
	add_path(joinrel, &cp->path);
}

/*
 * The private list of the plan, all ints: the join type and the bytes the planner expects the build side to take (two halves), the number of keys, for each its column in the outer child, its column in
 * the inner child and its type; the number of columns of the scan tuple and, for each, 0 or 1 (the outer or inner child) and its column in that child.
 */
static Plan *
join_plan_path(PlannerInfo *root, RelOptInfo *rel, CustomPath *best, List *tlist, List *clauses, List *custom_plans)
{
	CustomScan *cs = makeNode(CustomScan);
	const List *priv = best->custom_private;
	const Plan *op = linitial(custom_plans);
	const Plan *ip = lsecond(custom_plans);
	const List *head = linitial(priv);
	List	   *ints = list_make4_int(linitial_int(head), lsecond_int(head), lthird_int(head), list_length(lsecond(priv)));
	ListCell   *ol,
			   *il;

	forboth(ol, lsecond(priv), il, lthird(priv))
	{
		const Var  *ok = lfirst(ol);
		const Var  *ik = lfirst(il);

		ints = list_concat(ints, list_make3_int(sift_v6_child_resno(op->targetlist, ok), sift_v6_child_resno(ip->targetlist, ik), (int) exprType((Node *) ok)));
	}
	ints = lappend_int(ints, list_length(tlist));
	foreach_node(TargetEntry, te, tlist)
	{
		const Var  *v = (const Var *) te->expr;
		const int	o = sift_v6_child_resno(op->targetlist, v);

		ints = list_concat(ints, o > 0 ? list_make2_int(0, o) : list_make2_int(1, sift_v6_child_resno(ip->targetlist, v)));
	}
	cs->scan.plan.targetlist = tlist;
	cs->scan.scanrelid = 0;
	cs->flags = best->flags;
	cs->custom_plans = custom_plans;
	cs->custom_scan_tlist = tlist;
	cs->custom_private = list_make1(ints);
	cs->methods = &sift_agg_v6_join_plan_methods;
	return &cs->scan.plan;
}

/* ---- the node ---- */

typedef struct V6Join
{
	CustomScanState css;
	SiftJoinType type;
	uint64		est_build_bytes;
	int			nkeys;
	AttrNumber	okey[JOIN_MAX_KEYS];
	AttrNumber	ikey[JOIN_MAX_KEYS];
	Oid			ktyp[JOIN_MAX_KEYS];
	int			nout;
	bool	   *inner;			/* of each column of the scan tuple: its child is the inner one */
	AttrNumber *resno;
	PlanState  *ops;
	PlanState  *ips;
	TupleTableSlot *oslot;
	TupleTableSlot *islot;
	SiftV6Store out;
	SiftDistinctKey kb;
	SiftGraceJoin *gj;
	SiftGraceStats stats;
	Datum	   *vals;
	bool	   *nulls;
} V6Join;

static void
join_begin(CustomScanState *node, EState *estate, int eflags)
{
	V6Join	   *st = (V6Join *) node;
	const CustomScan *cs = (const CustomScan *) node->ss.ps.plan;
	const List *ints = linitial(cs->custom_private);
	int			at = 4;

	st->ops = ExecInitNode(linitial(cs->custom_plans), estate, eflags);
	st->ips = ExecInitNode(lsecond(cs->custom_plans), estate, eflags);
	node->custom_ps = list_make2(st->ops, st->ips);
	st->type = (SiftJoinType) list_nth_int(ints, 0);
	st->est_build_bytes = ((uint64) (uint32) list_nth_int(ints, 2) << 32) | (uint32) list_nth_int(ints, 1);
	st->nkeys = list_nth_int(ints, 3);
	for (int k = 0; k < st->nkeys; k++, at += 3)
	{
		st->okey[k] = (AttrNumber) list_nth_int(ints, at);
		st->ikey[k] = (AttrNumber) list_nth_int(ints, at + 1);
		st->ktyp[k] = (Oid) list_nth_int(ints, at + 2);
	}
	st->nout = list_nth_int(ints, at++);
	st->inner = palloc(Max(st->nout, 1) * sizeof(bool));
	st->resno = palloc(Max(st->nout, 1) * sizeof(AttrNumber));
	for (int i = 0; i < st->nout; i++, at += 2)
	{
		st->inner[i] = list_nth_int(ints, at) != 0;
		st->resno[i] = (AttrNumber) list_nth_int(ints, at + 1);
	}
	st->oslot = MakeSingleTupleTableSlot(ExecGetResultType(st->ops), &TTSOpsMinimalTuple);
	st->islot = MakeSingleTupleTableSlot(ExecGetResultType(st->ips), &TTSOpsMinimalTuple);
	st->vals = palloc(Max(st->nout, 1) * sizeof(Datum));
	st->nulls = palloc(Max(st->nout, 1) * sizeof(bool));
}

static void
load_side(TupleTableSlot *slot, const uint8 *bytes, uint32 len)
{
	MinimalTuple mt = palloc(len);

	memcpy(mt, bytes, len);
	ExecStoreMinimalTuple(mt, slot, false);
	slot_getallattrs(slot);
}

static void
join_emit(void *arg, const uint8 *probe, uint32 plen, const uint8 *build, uint32 blen)
{
	V6Join	   *st = arg;
	MemoryContext old = MemoryContextSwitchTo(st->out.row_cx);

	load_side(st->oslot, probe, plen);
	if (build != NULL)
		load_side(st->islot, build, blen);
	for (int i = 0; i < st->nout; i++)
	{
		const TupleTableSlot *s = st->inner[i] ? st->islot : st->oslot;

		st->nulls[i] = (st->inner[i] && build == NULL) || s->tts_isnull[st->resno[i] - 1];
		st->vals[i] = st->nulls[i] ? (Datum) 0 : s->tts_values[st->resno[i] - 1];
	}
	MemoryContextSwitchTo(old);
	sift_v6_store_put(&st->out, st->css.ss.ss_ScanTupleSlot->tts_tupleDescriptor, st->vals, st->nulls);
}

/* the key of the tuple in the slot as bytes: true when a column of it is NULL, which no key equals */
static bool
key_of(V6Join *st, TupleTableSlot *slot, const AttrNumber *cols)
{
	Datum		d[JOIN_MAX_KEYS];
	bool		n[JOIN_MAX_KEYS];
	bool		any_null = false;

	for (int k = 0; k < st->nkeys; k++)
	{
		d[k] = slot_getattr(slot, cols[k], &n[k]);
		any_null |= n[k];
	}
	if (any_null)
	{
		sift_dkey_reset(&st->kb);
		sift_dkey_null(&st->kb);
	}
	else
		sift_distinct_row_key(&st->kb, st->nkeys, st->ktyp, d, n);
	return any_null;
}

static void
feed(V6Join *st, PlanState *ps, const AttrNumber *cols, bool build)
{
	for (;;)
	{
		TupleTableSlot *slot = ExecProcNode(ps);
		MemoryContext old;
		MinimalTuple mt;
		bool		free_mt,
					key_null;

		if (TupIsNull(slot))
			return;
		CHECK_FOR_INTERRUPTS();
		old = MemoryContextSwitchTo(st->css.ss.ps.ps_ExprContext->ecxt_per_tuple_memory);
		key_null = key_of(st, slot, cols);
		mt = ExecFetchSlotMinimalTuple(slot, &free_mt);
		if (build)
			sift_gjoin_build(st->gj, st->kb.buf, st->kb.len, key_null, mt, mt->t_len);
		else
			sift_gjoin_probe(st->gj, st->kb.buf, st->kb.len, key_null, mt, mt->t_len);
		MemoryContextSwitchTo(old);
		ResetExprContext(st->css.ss.ps.ps_ExprContext);
	}
}

static void
join_run(V6Join *st, EState *estate)
{
	sift_v6_store_start(&st->out, estate);
	sift_dkey_init(&st->kb, st->out.cx);
	st->gj = sift_gjoin_create(st->out.cx, st->type, (uint64) work_mem * 1024, st->est_build_bytes, join_emit, st);
	feed(st, st->ips, st->ikey, true);
	sift_gjoin_build_done(st->gj);
	feed(st, st->ops, st->okey, false);
	sift_gjoin_finish(st->gj);
	st->stats = *sift_gjoin_stats(st->gj);
	sift_gjoin_free(st->gj);
	st->gj = NULL;
	sift_dkey_free(&st->kb);
	st->out.ran = true;
}

static TupleTableSlot *
join_next(ScanState *ss)
{
	V6Join	   *st = (V6Join *) ss;

	if (!st->out.ran)
		join_run(st, ss->ps.state);
	return sift_v6_store_get(&st->out, ss->ss_ScanTupleSlot);
}

static bool
join_recheck(ScanState *ss, TupleTableSlot *slot)
{
	return true;
}

static TupleTableSlot *
join_exec(CustomScanState *node)
{
	return ExecScan(&node->ss, join_next, join_recheck);
}

static void
join_end(CustomScanState *node)
{
	V6Join	   *st = (V6Join *) node;

	sift_v6_store_end(&st->out);
	ExecDropSingleTupleTableSlot(st->oslot);
	ExecDropSingleTupleTableSlot(st->islot);
	ExecEndNode(st->ops);
	ExecEndNode(st->ips);
}

/* the same parameters give the same rows: a scan again reads them from their start; other parameters make the join run again over children that are scanned again */
static void
join_rescan(CustomScanState *node)
{
	V6Join	   *st = (V6Join *) node;
	const bool	changed = node->ss.ps.chgParam != NULL;

	if (changed)
		foreach_ptr(PlanState, child, node->custom_ps)
		{
			UpdateChangedParamSet(child, node->ss.ps.chgParam);
			if (child->chgParam == NULL)
				ExecReScan(child);
		}
	sift_v6_store_rescan(&st->out, changed);
}

static void
join_explain(CustomScanState *node, List *ancestors, ExplainState *es)
{
	V6Join	   *st = (V6Join *) node;
	static const char *const names[] = {"Inner", "Left", "Semi", "Anti"};

	ExplainPropertyText("Join Type", names[st->type], es);
	if (st->out.ran)
	{
		sift_v6_explain_count(es, "Build Rows", (int64) st->stats.build_rows);
		sift_v6_explain_count(es, "Probe Rows", (int64) st->stats.probe_rows);
		sift_v6_explain_count(es, "Partitions", (int64) st->stats.partitions);
		sift_v6_explain_count(es, "Spilled Bytes", (int64) st->stats.spilled_bytes);
		sift_v6_explain_count(es, "Peak Bytes", (int64) st->stats.peak_bytes);
	}
}

static const CustomExecMethods join_exec_methods = {
	.CustomName = "SiftAggV6Join",
	.BeginCustomScan = join_begin,
	.ExecCustomScan = join_exec,
	.EndCustomScan = join_end,
	.ReScanCustomScan = join_rescan,
	.ExplainCustomScan = join_explain,
};

static Node *
join_create_state(CustomScan *cscan)
{
	V6Join	   *st = (V6Join *) newNode(sizeof(V6Join), T_CustomScanState);

	st->css.methods = &join_exec_methods;
	st->css.slotOps = &TTSOpsMinimalTuple;
	return (Node *) st;
}

const CustomScanMethods sift_agg_v6_join_plan_methods = {"SiftAggV6Join", join_create_state};
