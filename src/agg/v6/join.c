/*
 * agg/v6/join.c - the partitioned (Grace) hash join of SiftAggV6: the path of an inner, left, semi or anti join of columns compared for equality whose build side does not fit in half of work_mem, and
 * the node that runs it through v6_grace_join.h instead of the batches of PostgreSQL's hash join.
 *
 * The path takes the cheapest unparameterized paths of the two sides as its children (PostgreSQL plans them, with every scan and filter of its own), the outer side is probed and the inner one built. The
 * keys are plain columns of the children, which the node encodes as the exact bytes of v6_distinct_pg.h (the integer types are one key type, the float types another: two columns of other types are no
 * key); a join clause that is anything else is no join for this node and the path is not offered. Each child tuple is the payload of its row, a minimal tuple the node stores in the join's files and
 * reads back, so the join holds no more than its share of work_mem whatever the rows. The rows it makes are the tuple of the scan the planner asked for: the columns of the outer child and then those of
 * the inner one, the inner ones NULL for a probe row of a left join that matched nothing. They are given as they are made: the node reads the probe side a row at a time and hands out the rows of each
 * row before it reads the next (what the partitions that went to files join is given after the last probe row), so a join of many rows holds a batch of them, not all.
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
#include "utils/tuplestore.h"

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

	if (!sift_store_ops_any())
		return false;
	while ((i = bms_next_member(relids, i)) >= 0)
	{
		const RelOptInfo *r = root->simple_rel_array[i];
		const RangeTblEntry *rte = planner_rt_fetch(i, root);

		if (r == NULL || r->reloptkind != RELOPT_BASEREL)
			continue;
		if (rte->rtekind == RTE_RELATION && sift_v6_rel_owned(rte->relid, true))
			return true;
		foreach_ptr(IndexOptInfo, ix, r->indexlist)
			if (!ix->hypothetical && sift_v6_rel_owned(ix->indexoid, false))
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
	MemoryContext cx;			/* one run of the join: its table, its keys and its queue */
	MemoryContext row_cx;		/* what one output tuple takes to be built */
	Tuplestorestate *wr;		/* the rows made, filling */
	Tuplestorestate *rd;		/* the rows being given: the batch that was full before it */
	uint64		nwr;
	bool		started;		/* the build side is in */
	bool		probed;			/* the probe side is read and the partitions that went to files are joined */
	MinimalTuple pbuf;			/* the probe and build tuples of an output row, rebuilt in place for each row */
	MinimalTuple bbuf;
	Size		pcap;
	Size		bcap;
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

/* the tuple of a side of an output row, copied to the buffer (aligned, grown when a tuple is longer) the slot reads it from */
static void
load_side(MemoryContext cx, TupleTableSlot *slot, MinimalTuple *buf, Size *cap, const uint8 *bytes, uint32 len)
{
	if (len > *cap)
	{
		*cap = Max((Size) len, *cap * 2);
		*buf = *buf == NULL ? MemoryContextAlloc(cx, *cap) : repalloc(*buf, *cap);
	}
	memcpy(*buf, bytes, len);
	ExecStoreMinimalTuple(*buf, slot, false);
	slot_getallattrs(slot);
}

static void
join_emit(void *arg, const uint8 *probe, uint32 plen, const uint8 *build, uint32 blen)
{
	V6Join	   *st = arg;
	MemoryContext old = MemoryContextSwitchTo(st->row_cx);

	load_side(st->cx, st->oslot, &st->pbuf, &st->pcap, probe, plen);
	if (build != NULL)
		load_side(st->cx, st->islot, &st->bbuf, &st->bcap, build, blen);
	for (int i = 0; i < st->nout; i++)
	{
		const TupleTableSlot *s = st->inner[i] ? st->islot : st->oslot;

		st->nulls[i] = (st->inner[i] && build == NULL) || s->tts_isnull[st->resno[i] - 1];
		st->vals[i] = st->nulls[i] ? (Datum) 0 : s->tts_values[st->resno[i] - 1];
	}
	MemoryContextSwitchTo(old);
	tuplestore_putvalues(st->wr, st->css.ss.ss_ScanTupleSlot->tts_tupleDescriptor, st->vals, st->nulls);
	MemoryContextReset(st->row_cx);
	st->nwr++;
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

/* the tuple of the child as a row of the join: the build side into the table, the probe side joined to it (what it matches is made into rows by join_emit); the key and the copy of the tuple die with the row, what the join allocates (the files of its partitions) lives with the join */
static void
take_row(V6Join *st, TupleTableSlot *slot, const AttrNumber *cols, bool build)
{
	MemoryContext old = MemoryContextSwitchTo(st->css.ss.ps.ps_ExprContext->ecxt_per_tuple_memory);
	bool		free_mt;
	const bool	key_null = key_of(st, slot, cols);
	MinimalTuple mt = ExecFetchSlotMinimalTuple(slot, &free_mt);

	MemoryContextSwitchTo(st->cx);
	if (build)
		sift_gjoin_build(st->gj, st->kb.buf, st->kb.len, key_null, mt, mt->t_len);
	else
		sift_gjoin_probe(st->gj, st->kb.buf, st->kb.len, key_null, mt, mt->t_len);
	MemoryContextSwitchTo(old);
	ResetExprContext(st->css.ss.ps.ps_ExprContext);
}

static Tuplestorestate *
queue_begin(EState *estate)
{
	MemoryContext old = MemoryContextSwitchTo(estate->es_query_cxt);
	Tuplestorestate *q = tuplestore_begin_heap(false, false, work_mem);

	MemoryContextSwitchTo(old);
	return q;
}

static void
join_start(V6Join *st, EState *estate)
{
	TupleTableSlot *slot = NULL;

	st->cx = sift_mem_allocset_create(estate->es_query_cxt, "sift agg v6 join");
	st->row_cx = AllocSetContextCreate(st->cx, "sift agg v6 join tuple", ALLOCSET_SMALL_SIZES);
	st->wr = queue_begin(estate);
	sift_dkey_init(&st->kb, st->cx);
	st->gj = sift_gjoin_create(st->cx, st->type, (uint64) work_mem * 1024, st->est_build_bytes, join_emit, st);
	for (slot = ExecProcNode(st->ips); !TupIsNull(slot); slot = ExecProcNode(st->ips))
	{
		CHECK_FOR_INTERRUPTS();
		take_row(st, slot, st->ikey, true);
	}
	sift_gjoin_build_done(st->gj);
	st->started = true;
}

static void
join_probe_done(V6Join *st)
{
	sift_gjoin_finish(st->gj);
	st->stats = *sift_gjoin_stats(st->gj);
	sift_gjoin_free(st->gj);
	st->gj = NULL;
	sift_dkey_free(&st->kb);
	st->probed = true;
}

/* the rows of probe rows until JOIN_BATCH of them are made (or the probe side ends) are the next batch to give */
#define JOIN_BATCH	1024

static void
join_fill(V6Join *st)
{
	while (st->nwr < JOIN_BATCH && !st->probed)
	{
		TupleTableSlot *slot = ExecProcNode(st->ops);

		if (TupIsNull(slot))
			join_probe_done(st);
		else
		{
			CHECK_FOR_INTERRUPTS();
			take_row(st, slot, st->okey, false);
		}
	}
	st->rd = st->wr;
	st->wr = st->probed ? NULL : queue_begin(st->css.ss.ps.state);
	st->nwr = 0;
}

static TupleTableSlot *
join_next(ScanState *ss)
{
	V6Join	   *st = (V6Join *) ss;

	if (!st->started)
		join_start(st, ss->ps.state);
	for (;;)
	{
		if (st->rd != NULL && tuplestore_gettupleslot(st->rd, true, false, ss->ss_ScanTupleSlot))
			return ss->ss_ScanTupleSlot;
		if (st->rd != NULL)
			tuplestore_end(st->rd);
		st->rd = NULL;
		if (st->wr == NULL)
			return NULL;
		join_fill(st);
	}
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
join_reset(V6Join *st)
{
	if (st->rd != NULL)
		tuplestore_end(st->rd);
	if (st->wr != NULL)
		tuplestore_end(st->wr);
	if (st->gj != NULL)
		sift_gjoin_free(st->gj);
	if (st->cx != NULL)
		MemoryContextDelete(st->cx);
	st->rd = st->wr = NULL;
	st->gj = NULL;
	st->cx = st->row_cx = NULL;
	st->pbuf = st->bbuf = NULL;
	st->pcap = st->bcap = 0;
	st->nwr = 0;
	st->started = st->probed = false;
}

static void
join_end(CustomScanState *node)
{
	V6Join	   *st = (V6Join *) node;

	join_reset(st);
	ExecDropSingleTupleTableSlot(st->oslot);
	ExecDropSingleTupleTableSlot(st->islot);
	ExecEndNode(st->ops);
	ExecEndNode(st->ips);
}

/* a scan again runs the join again over children that are scanned again: the rows are given as they are made and are not kept */
static void
join_rescan(CustomScanState *node)
{
	V6Join	   *st = (V6Join *) node;

	foreach_ptr(PlanState, child, node->custom_ps)
	{
		if (node->ss.ps.chgParam != NULL)
			UpdateChangedParamSet(child, node->ss.ps.chgParam);
		if (child->chgParam == NULL)
			ExecReScan(child);
	}
	join_reset(st);
}

static void
join_explain(CustomScanState *node, List *ancestors, ExplainState *es)
{
	V6Join	   *st = (V6Join *) node;
	static const char *const names[] = {"Inner", "Left", "Semi", "Anti"};

	ExplainPropertyText("Join Type", names[st->type], es);
	if (st->started)
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
