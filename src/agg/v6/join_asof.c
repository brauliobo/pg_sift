/*
 * agg/v6/join_asof.c - the ASOF join of SiftAggV6: the path of PostgreSQL's idiom
 *
 *   FROM fact f LEFT | INNER JOIN LATERAL (SELECT .. FROM dim d WHERE d.k = f.k AND d.ts <= f.ts ORDER BY d.ts DESC LIMIT 1) x ON true
 *
 * (and the mirror with >= and ASC; < and > for the strict forms) and the node that runs it as one merge (v6_asof.h) instead of an index probe of dim for each fact row. dim must be a table with an
 * index of format major 2 (or be a sift_table) whose parts are sorted by (k, ts): its batches come from the format in that order, and what the sub-select returns of the row that matches is fetched by the row's tag. The
 * fact rows come from the plan PostgreSQL makes for the fact side and are sorted by (k, ts) by the node (a tuplesort, which spills past work_mem), so that the merge reads dim once and the fact rows once.
 * The sub-select must be exactly the idiom: a clause of dim that is none of the two, a column that is an expression, an ON clause or another sort leaves the join to PostgreSQL (v6_plan_shape.h).
 */
/* check-memory: allow-allocs every array here follows the columns of one statement, not the rows of the tables */
#include "postgres.h"

#include <math.h>

#include "access/genam.h"
#include "access/htup_details.h"
#include "access/relation.h"
#include "access/table.h"
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
#include "utils/lsyscache.h"
#include "utils/rel.h"
#include "utils/tuplesort.h"
#include "utils/typcache.h"

#include "agg/agg.h"
#include "agg/plan/safety.h"
#include "agg/v6/v6_asof.h"
#include "agg/v6/v6_batch.h"
#include "agg/v6/v6_join.h"
#include "agg/v6/v6_plan_shape.h"
#include "agg/v6/v6_vexpr.h"

typedef struct Idiom
{
	SiftAsofShape shape;
	Var		   *dim_k;
	Var		   *dim_ts;
	Var		   *fact_k;
	Var		   *fact_ts;
	List	   *sub_attno;		/* of each column of the sub-select, the attribute of dim it is (0: a column the join does not give) */
} Idiom;

static bool
asof_type(Oid t)
{
	return t == INT4OID || t == INT8OID || t == DATEOID || t == TIMESTAMPOID || t == TIMESTAMPTZOID;
}

static bool
is_dim_var(const Node *n)
{
	return IsA(n, Var) && ((const Var *) n)->varno == 1 && ((const Var *) n)->varlevelsup == 0;
}

static bool
is_fact_var(const Node *n, const RelOptInfo *outerrel)
{
	return IsA(n, Var) && ((const Var *) n)->varlevelsup == 1 && bms_is_member(((const Var *) n)->varno, outerrel->relids);
}

/* one clause of the sub-select: dim.k = fact.k or dim.ts <op> fact.ts, in either order */
static void
classify_qual(const RelOptInfo *outerrel, const Node *q, Idiom *id)
{
	const OpExpr *op = (const OpExpr *) q;
	Node	   *l,
			   *r;
	Oid			opno;

	if (!IsA(q, OpExpr) || list_length(op->args) != 2)
	{
		id->shape.nother++;
		return;
	}
	l = linitial(op->args);
	r = lsecond(op->args);
	opno = op->opno;
	if (is_fact_var(l, outerrel) && is_dim_var(r))
	{
		Node	   *t = l;

		l = r;
		r = t;
		opno = get_commutator(opno);
	}
	if (!OidIsValid(opno) || !is_dim_var(l) || !is_fact_var(r, outerrel) || exprType(l) != exprType(r) || !asof_type(exprType(l)))
	{
		id->shape.nother++;
		return;
	}
	foreach_int(strategy, sift_op_strategies(opno))
	{
		if (strategy == BTEqualStrategyNumber)
		{
			id->shape.nkey_eq++;
			id->dim_k = (Var *) l;
			id->fact_k = (Var *) r;
			return;
		}
		if (strategy == BTLessEqualStrategyNumber || strategy == BTLessStrategyNumber || strategy == BTGreaterEqualStrategyNumber || strategy == BTGreaterStrategyNumber)
		{
			id->shape.nts_cmp++;
			id->shape.ts_cmp = strategy == BTLessEqualStrategyNumber ? SAC_LE : strategy == BTLessStrategyNumber ? SAC_LT : strategy == BTGreaterEqualStrategyNumber ? SAC_GE : SAC_GT;
			id->dim_ts = (Var *) l;
			id->fact_ts = (Var *) r;
			return;
		}
	}
	id->shape.nother++;
}

/* the sub-select as the shape describes it; false when it is not a sub-select of one plain table whose columns are all columns of it */
static bool
describe(const RelOptInfo *outerrel, const Query *sub, Idiom *id)
{
	const RangeTblEntry *rte;
	const Const *limit;

	id->shape.single_table = list_length(sub->rtable) == 1 && sub->groupClause == NIL && !sub->hasAggs && !sub->hasWindowFuncs && sub->distinctClause == NIL &&
		sub->setOperations == NULL && !sub->hasTargetSRFs && sub->cteList == NIL && sub->groupingSets == NIL && sub->havingQual == NULL && sub->rowMarks == NIL &&
		list_length(sub->jointree->fromlist) == 1 && IsA(linitial(sub->jointree->fromlist), RangeTblRef);
	if (!id->shape.single_table)
		return true;
	rte = linitial(sub->rtable);
	id->shape.single_table = rte->rtekind == RTE_RELATION && rte->relkind == RELKIND_RELATION && !rte->inh && rte->tablesample == NULL;
	limit = (const Const *) sub->limitCount;
	id->shape.limit_one = limit != NULL && IsA(limit, Const) && !limit->constisnull && limit->consttype == INT8OID && DatumGetInt64(limit->constvalue) == 1 &&
		sub->limitOption == LIMIT_OPTION_COUNT;
	id->shape.has_offset = sub->limitOffset != NULL;
	foreach_ptr(Node, q, make_ands_implicit((Expr *) sub->jointree->quals))
		classify_qual(outerrel, q, id);
	id->shape.nsort = list_length(sub->sortClause);
	if (id->shape.nsort == 1 && id->dim_ts != NULL)
	{
		const SortGroupClause *sgc = linitial(sub->sortClause);
		const Node *e = (const Node *) get_sortgroupclause_expr((SortGroupClause *) sgc, sub->targetList);
		bool		reverse;

		id->shape.sort_on_ts = is_dim_var(e) && ((const Var *) e)->varattno == id->dim_ts->varattno && OidIsValid(get_equality_op_for_ordering_op(sgc->sortop, &reverse));
		id->shape.sort_desc = OidIsValid(get_equality_op_for_ordering_op(sgc->sortop, &reverse)) && reverse;
	}
	id->shape.types_ok = id->dim_k != NULL && id->dim_ts != NULL && asof_type(id->dim_k->vartype) && asof_type(id->dim_ts->vartype);
	foreach_node(TargetEntry, te, sub->targetList)
	{
		if (!te->resjunk && !is_dim_var((Node *) te->expr))
			return false;
		id->sub_attno = lappend_int(id->sub_attno, te->resjunk ? 0 : ((Var *) te->expr)->varattno);
	}
	return true;
}

/* what the join reads dim through: its sift_table, or an index of it */
typedef struct DimHost
{
	Oid			oid;
	bool		is_table;
} DimHost;

/* the columns of the input that hold k and ts, and the parts sorted by them */
static bool
sorted_input(SiftV6Input *in, const Idiom *id)
{
	const int	ks = sift_v6_input_add_var(in, id->dim_k);
	const int	ts = sift_v6_input_add_var(in, id->dim_ts);
	int			atts[2];

	if (ks < 0 || ts < 0)
		return false;
	atts[0] = in->col[ks].attno;
	atts[1] = in->col[ts].attno;
	return sift_v6_format()->sorted_by(in->indexoid, atts, 2);
}

static bool
sorted_table(const RangeTblEntry *rte, const Idiom *id)
{
	SiftV6Input in;
	Relation	tbl;

	if (!sift_v6_format()->owns_rel(rte->relid, true))
		return false;
	tbl = table_open(rte->relid, AccessShareLock);
	sift_v6_input_init_table(&in, tbl);
	table_close(tbl, NoLock);
	return sorted_input(&in, id);
}

static bool
sorted_index(const IndexOptInfo *ix, const Idiom *id)
{
	SiftV6Input in;

	if (ix->hypothetical || ix->indpred != NIL || ix->indexprs != NIL || !sift_v6_format()->owns_rel(ix->indexoid, false))
		return false;
	sift_v6_input_init_info(&in, ix);
	return sorted_input(&in, id);
}

/* the sift_table dim, or the index of it, whose parts are sorted by (k, ts); false when there is none */
static bool
sorted_host(PlannerInfo *subroot, const Idiom *id, DimHost *host)
{
	const RangeTblEntry *rte = planner_rt_fetch(1, subroot);

	if (sorted_table(rte, id))
	{
		*host = (DimHost) {.oid = rte->relid,.is_table = true};
		return true;
	}
	foreach_ptr(IndexOptInfo, ix, find_base_rel(subroot, 1)->indexlist)
		if (sorted_index(ix, id))
		{
			*host = (DimHost) {.oid = ix->indexoid,.is_table = false};
			return true;
		}
	return false;
}

/* every column the join gives is a column of the fact child or of the sub-select (one of its columns that is a column of dim) */
static bool
resolvable(const RelOptInfo *joinrel, const RelOptInfo *innerrel, const Path *fact, const Idiom *id)
{
	foreach_ptr(Expr, e, joinrel->reltarget->exprs)
	{
		const Var  *v = (const Var *) e;

		if (!IsA(e, Var) || v->varlevelsup != 0)
			return false;
		if (v->varno == innerrel->relid)
		{
			if (v->varattno < 1 || v->varattno > list_length(id->sub_attno) || list_nth_int(id->sub_attno, v->varattno - 1) == 0)
				return false;
		}
		else if (!sift_v6_target_has(fact->pathtarget, v))
			return false;
	}
	return true;
}

static Plan *asof_plan_path(PlannerInfo *root, RelOptInfo *rel, CustomPath *best, List *tlist, List *clauses, List *custom_plans);

static const CustomPathMethods asof_path_methods = {"SiftAggV6Asof", asof_plan_path, NULL};

void
sift_agg_v6_asof_paths(PlannerInfo *root, RelOptInfo *joinrel, RelOptInfo *outerrel, RelOptInfo *innerrel, JoinType jointype, JoinPathExtraData *extra)
{
	Idiom		id = {{0}};
	SiftAsofSpec spec;
	const Path *fact = outerrel->cheapest_total_path;
	const RangeTblEntry *rte;
	DimHost		host;
	CustomPath *cp;
	double		n,
				m,
				startup;

	if (!sift_enable_agg || root->parse->commandType != CMD_SELECT || root->rowMarks != NIL || fact == NULL || fact->param_info != NULL || innerrel->reloptkind != RELOPT_BASEREL ||
		innerrel->subroot == NULL || !bms_overlap(innerrel->lateral_relids, outerrel->relids) || outerrel->lateral_relids != NULL)
		return;
	rte = planner_rt_fetch(innerrel->relid, root);
	if (rte->rtekind != RTE_SUBQUERY)
		return;
	id.shape.type = jointype == JOIN_LEFT ? SSJ_LEFT : jointype == JOIN_INNER ? SSJ_INNER : SSJ_FULL;
	id.shape.on_true = extra->restrictlist == NIL;
	if (!describe(outerrel, rte->subquery, &id) || sift_shape_asof(&id.shape, &spec) != SSV_OK)
		return;
	if (agg_rte_has_rls(innerrel->subroot, planner_rt_fetch(1, innerrel->subroot)) || !sorted_host(innerrel->subroot, &id, &host) || !resolvable(joinrel, innerrel, fact, &id) ||
		!sift_v6_target_has(fact->pathtarget, id.fact_k) || !sift_v6_target_has(fact->pathtarget, id.fact_ts))
		return;

	n = Max(fact->rows, 1.0);
	m = find_base_rel(innerrel->subroot, 1)->tuples;
	startup = fact->total_cost + 2.0 * cpu_operator_cost * n * ceil(log2(n + 1.0));
	cp = makeNode(CustomPath);
	cp->path.pathtype = T_CustomScan;
	cp->path.parent = joinrel;
	cp->path.pathtarget = joinrel->reltarget;
	cp->path.rows = joinrel->rows;
	cp->path.startup_cost = startup;
	cp->path.total_cost = startup + cpu_tuple_cost * 0.05 * Max(m, 1.0) + (cpu_tuple_cost + cpu_index_tuple_cost) * n + cpu_tuple_cost * joinrel->rows;
	cp->custom_paths = list_make1((Path *) fact);
	cp->custom_private = list_make3(lappend_int(list_make5_int((int) spec.dir, spec.strict, jointype == JOIN_LEFT, (int) host.oid, (int) innerrel->relid), host.is_table),
									list_make4(id.dim_k, id.dim_ts, id.fact_k, id.fact_ts), id.sub_attno);
	cp->methods = &asof_path_methods;
	add_path(joinrel, &cp->path);
}

/*
 * The private list of the plan, all ints: the direction, whether the comparison is strict, whether the join is a left one, the index of dim (its table, when the next int is set), the range table index
 * of dim, whether dim is read as a table, the attributes of dim and the columns of the fact child
 * of the keys and of the times, their types, and the columns of the scan tuple: for each 0 and the column of the fact child, or 1 and the attribute of dim.
 */
static Plan *
asof_plan_path(PlannerInfo *root, RelOptInfo *rel, CustomPath *best, List *tlist, List *clauses, List *custom_plans)
{
	CustomScan *cs = makeNode(CustomScan);
	const List *head = linitial(best->custom_private);
	const List *vars = lsecond(best->custom_private);
	const List *sub_attno = lthird(best->custom_private);
	const Plan *fact = linitial(custom_plans);
	const Var  *dk = linitial(vars);
	const Var  *dt = lsecond(vars);
	const Var  *fk = lthird(vars);
	const Var  *ft = lfourth(vars);
	List	   *ints = list_copy(head);

	ints = list_concat(ints, list_make5_int(dk->varattno, dt->varattno, sift_v6_child_resno(fact->targetlist, fk), sift_v6_child_resno(fact->targetlist, ft), (int) dk->vartype));
	ints = lappend_int(ints, (int) dt->vartype);
	ints = lappend_int(ints, list_length(tlist));
	foreach_node(TargetEntry, te, tlist)
	{
		const Var  *v = (const Var *) te->expr;

		if (v->varno == (Index) list_nth_int(head, 4))
			ints = list_concat(ints, list_make2_int(1, list_nth_int(sub_attno, v->varattno - 1)));
		else
			ints = list_concat(ints, list_make2_int(0, sift_v6_child_resno(fact->targetlist, v)));
	}
	cs->scan.plan.targetlist = tlist;
	cs->scan.scanrelid = 0;
	cs->flags = best->flags;
	cs->custom_plans = custom_plans;
	cs->custom_scan_tlist = tlist;
	cs->custom_private = list_make1(ints);
	cs->methods = &sift_agg_v6_asof_plan_methods;
	root->glob->relationOids = lappend_oid(root->glob->relationOids, (Oid) list_nth_int(head, 3));
	return &cs->scan.plan;
}

/* ---- the node ---- */

typedef struct V6Asof
{
	CustomScanState css;
	SiftAsofSpec spec;
	bool		left;
	Oid			indexoid;		/* the table of dim when is_table */
	bool		is_table;
	AttrNumber	dim_k,
				dim_ts;
	AttrNumber	fact_k,
				fact_ts;
	Oid			ktyp,
				tstyp;
	int			nout;
	bool	   *dim;			/* of each column of the scan tuple: it is a column of dim */
	AttrNumber *col;
	int			nfetch;			/* the attributes of dim the join gives */
	AttrNumber *fetch;
	int		   *fetch_at;		/* of each column of the scan tuple that is one of dim: its place among fetch */
	PlanState  *fps;
	TupleTableSlot *fslot;
	SiftV6Store out;
	SiftAsofStats stats;
	Datum	   *vals;
	bool	   *nulls;
	Datum	   *dvals;
	bool	   *dnulls;
} V6Asof;

static void
asof_begin(CustomScanState *node, EState *estate, int eflags)
{
	V6Asof	   *st = (V6Asof *) node;
	const CustomScan *cs = (const CustomScan *) node->ss.ps.plan;
	const List *ints = linitial(cs->custom_private);
	int			at;

	st->fps = ExecInitNode(linitial(cs->custom_plans), estate, eflags);
	node->custom_ps = list_make1(st->fps);
	st->spec = (SiftAsofSpec) {.dir = (SiftAsofDir) list_nth_int(ints, 0),.strict = list_nth_int(ints, 1) != 0};
	st->left = list_nth_int(ints, 2) != 0;
	st->indexoid = (Oid) list_nth_int(ints, 3);
	st->is_table = list_nth_int(ints, 5) != 0;
	st->dim_k = (AttrNumber) list_nth_int(ints, 6);
	st->dim_ts = (AttrNumber) list_nth_int(ints, 7);
	st->fact_k = (AttrNumber) list_nth_int(ints, 8);
	st->fact_ts = (AttrNumber) list_nth_int(ints, 9);
	st->ktyp = (Oid) list_nth_int(ints, 10);
	st->tstyp = (Oid) list_nth_int(ints, 11);
	st->nout = list_nth_int(ints, 12);
	st->dim = palloc(Max(st->nout, 1) * sizeof(bool));
	st->col = palloc(Max(st->nout, 1) * sizeof(AttrNumber));
	st->fetch = palloc(Max(st->nout, 1) * sizeof(AttrNumber));
	st->fetch_at = palloc(Max(st->nout, 1) * sizeof(int));
	at = 13;
	for (int i = 0; i < st->nout; i++, at += 2)
	{
		st->dim[i] = list_nth_int(ints, at) != 0;
		st->col[i] = (AttrNumber) list_nth_int(ints, at + 1);
		st->fetch_at[i] = -1;
		if (!st->dim[i])
			continue;
		for (int j = 0; j < st->nfetch && st->fetch_at[i] < 0; j++)
			if (st->fetch[j] == st->col[i])
				st->fetch_at[i] = j;
		if (st->fetch_at[i] < 0)
		{
			st->fetch[st->nfetch] = st->col[i];
			st->fetch_at[i] = st->nfetch++;
		}
	}
	st->fslot = MakeSingleTupleTableSlot(ExecGetResultType(st->fps), &TTSOpsMinimalTuple);
	st->vals = palloc(Max(st->nout, 1) * sizeof(Datum));
	st->nulls = palloc(Max(st->nout, 1) * sizeof(bool));
	st->dvals = palloc(Max(st->nfetch, 1) * sizeof(Datum));
	st->dnulls = palloc(Max(st->nfetch, 1) * sizeof(bool));
}

/* the batches of dim in the order of its parts, as the (k, ts) rows the merge reads */
typedef struct DimFeed
{
	SiftV6Source *src;
	int			kslot;
	int			tslot;
	SiftV6Batch b;
	int64		key[SIFT_VEC_LEN];
	int64		ts[SIFT_VEC_LEN];
	uint64		tag[SIFT_VEC_LEN];
	uint64		kvalid[SIFT_VEC_WORDS];
	uint64		tvalid[SIFT_VEC_WORDS];
} DimFeed;

static inline int64
int_of(const SiftVec *v, uint32 r)
{
	return v->type == SVEC_I32 ? ((const int32 *) v->data)[r] : ((const int64 *) v->data)[r];
}

static bool
dim_next(void *ctx, SiftAsofBatch *out)
{
	DimFeed    *f = ctx;

	while (f->src->next(f->src, &f->b))
	{
		const SiftVec *kv = &f->b.col[f->kslot];
		const SiftVec *tv = &f->b.col[f->tslot];
		uint32		n = 0;

		SIFT_INVARIANT(f->b.tag != NULL, "the rows of dim in the asof join have no tag");
		memset(f->kvalid, 0, sizeof(f->kvalid));
		memset(f->tvalid, 0, sizeof(f->tvalid));
		for (uint32 r = 0; r < f->b.n; r++)
		{
			if (f->b.mask != NULL && !((f->b.mask[r >> 6] >> (r & 63)) & 1))
				continue;
			f->key[n] = int_of(kv, r);
			f->ts[n] = int_of(tv, r);
			f->tag[n] = f->b.tag[r];
			f->kvalid[n >> 6] |= (uint64) sift_vec_valid_at(kv, r) << (n & 63);
			f->tvalid[n >> 6] |= (uint64) sift_vec_valid_at(tv, r) << (n & 63);
			n++;
		}
		if (n == 0)
			continue;
		*out = (SiftAsofBatch) {.n = n,.key = f->key,.ts = f->ts,.key_valid = f->kvalid,.ts_valid = f->tvalid,.tag = f->tag};
		return true;
	}
	return false;
}

static int64
int_datum(Oid typid, Datum d)
{
	return typid == INT4OID || typid == DATEOID ? (int64) DatumGetInt32(d) : DatumGetInt64(d);
}

/* the fact rows sorted by (k, ts), the NULLs last */
static Tuplesortstate *
sort_fact(V6Asof *st)
{
	const AttrNumber cols[2] = {st->fact_k, st->fact_ts};
	const Oid	typs[2] = {st->ktyp, st->tstyp};
	Oid			ops[2];
	Oid			colls[2] = {InvalidOid, InvalidOid};
	bool		nulls_first[2] = {false, false};
	Tuplesortstate *ts;

	for (int i = 0; i < 2; i++)
		ops[i] = lookup_type_cache(typs[i], TYPECACHE_LT_OPR)->lt_opr;
	ts = tuplesort_begin_heap(ExecGetResultType(st->fps), 2, (AttrNumber *) cols, ops, colls, nulls_first, work_mem, NULL, TUPLESORT_NONE);
	for (;;)
	{
		TupleTableSlot *slot = ExecProcNode(st->fps);

		if (TupIsNull(slot))
			break;
		CHECK_FOR_INTERRUPTS();
		tuplesort_puttupleslot(ts, slot);
	}
	tuplesort_performsort(ts);
	return ts;
}

/* the input over dim: the columns of its table, or of its index */
static void
dim_input(const V6Asof *st, SiftV6Input *in)
{
	Relation	rel = st->is_table ? table_open(st->indexoid, AccessShareLock) : index_open(st->indexoid, AccessShareLock);

	if (st->is_table)
		sift_v6_input_init_table(in, rel);
	else
		sift_v6_input_init_rel(in, rel);
	relation_close(rel, NoLock);
}

static void
asof_run(V6Asof *st, EState *estate)
{
	SiftV6Input in;
	DimFeed    *feed;
	SiftAsofStream *stream;
	Tuplesortstate *sorter;
	Var		   *kvar = makeVar(1, st->dim_k, st->ktyp, -1, InvalidOid, 0);
	Var		   *tvar = makeVar(1, st->dim_ts, st->tstyp, -1, InvalidOid, 0);
	TupleDesc	desc = st->css.ss.ss_ScanTupleSlot->tts_tupleDescriptor;

	sift_v6_store_start(&st->out, estate);
	dim_input(st, &in);
	feed = MemoryContextAllocZero(st->out.cx, sizeof(DimFeed));
	feed->kslot = sift_v6_input_add_var(&in, kvar);
	feed->tslot = sift_v6_input_add_var(&in, tvar);
	SIFT_INVARIANT(feed->kslot >= 0 && feed->tslot >= 0, "the asof join of relation %u does not read its keys", st->indexoid);
	feed->src = sift_v6_format()->open(estate, &in, NULL, true);
	stream = sift_asof_open(st->out.cx, st->spec, dim_next, feed);
	sorter = sort_fact(st);
	while (tuplesort_gettupleslot(sorter, true, false, st->fslot, NULL))
	{
		bool		knull,
					tnull;
		const Datum kd = slot_getattr(st->fslot, st->fact_k, &knull);
		const Datum td = slot_getattr(st->fslot, st->fact_ts, &tnull);
		const SiftAsofHit hit = sift_asof_probe(stream, knull ? 0 : int_datum(st->ktyp, kd), knull, tnull ? 0 : int_datum(st->tstyp, td), tnull);
		bool		fetched = false;

		CHECK_FOR_INTERRUPTS();
		if (!hit.found && !st->left)
			continue;
		if (hit.found && st->nfetch > 0)
			fetched = feed->src->fetch(feed->src, hit.tag, st->fetch, st->nfetch, st->dvals, st->dnulls);
		for (int i = 0; i < st->nout; i++)
		{
			if (!st->dim[i])
			{
				st->vals[i] = st->fslot->tts_values[st->col[i] - 1];
				st->nulls[i] = st->fslot->tts_isnull[st->col[i] - 1];
			}
			else
			{
				st->nulls[i] = !fetched || st->dnulls[st->fetch_at[i]];
				st->vals[i] = st->nulls[i] ? (Datum) 0 : st->dvals[st->fetch_at[i]];
			}
		}
		sift_v6_store_put(&st->out, desc, st->vals, st->nulls);
	}
	tuplesort_end(sorter);
	st->stats = *sift_asof_stats(stream);
	sift_asof_close(stream);
	feed->src->close(feed->src);
	st->out.ran = true;
}

static TupleTableSlot *
asof_next(ScanState *ss)
{
	V6Asof	   *st = (V6Asof *) ss;

	if (!st->out.ran)
		asof_run(st, ss->ps.state);
	return sift_v6_store_get(&st->out, ss->ss_ScanTupleSlot);
}

static bool
asof_recheck(ScanState *ss, TupleTableSlot *slot)
{
	return true;
}

static TupleTableSlot *
asof_exec(CustomScanState *node)
{
	return ExecScan(&node->ss, asof_next, asof_recheck);
}

static void
asof_end(CustomScanState *node)
{
	V6Asof	   *st = (V6Asof *) node;

	sift_v6_store_end(&st->out);
	ExecDropSingleTupleTableSlot(st->fslot);
	ExecEndNode(st->fps);
}

static void
asof_rescan(CustomScanState *node)
{
	V6Asof	   *st = (V6Asof *) node;
	const bool	changed = node->ss.ps.chgParam != NULL;

	if (changed)
	{
		UpdateChangedParamSet(st->fps, node->ss.ps.chgParam);
		if (st->fps->chgParam == NULL)
			ExecReScan(st->fps);
	}
	sift_v6_store_rescan(&st->out, changed);
}

static void
asof_explain(CustomScanState *node, List *ancestors, ExplainState *es)
{
	V6Asof	   *st = (V6Asof *) node;

	ExplainPropertyText(st->is_table ? "Table" : "Index", get_rel_name(st->indexoid), es);
	ExplainPropertyText("Direction", st->spec.dir == SIFT_ASOF_BACKWARD ? (st->spec.strict ? "Backward, strict" : "Backward") : (st->spec.strict ? "Forward, strict" : "Forward"), es);
	if (st->out.ran)
	{
		sift_v6_explain_count(es, "Fact Rows", (int64) st->stats.fact_rows);
		sift_v6_explain_count(es, "Dim Rows Read", (int64) st->stats.dim_rows);
		sift_v6_explain_count(es, "Matched", (int64) st->stats.matched);
	}
}

static const CustomExecMethods asof_exec_methods = {
	.CustomName = "SiftAggV6Asof",
	.BeginCustomScan = asof_begin,
	.ExecCustomScan = asof_exec,
	.EndCustomScan = asof_end,
	.ReScanCustomScan = asof_rescan,
	.ExplainCustomScan = asof_explain,
};

static Node *
asof_create_state(CustomScan *cscan)
{
	V6Asof	   *st = (V6Asof *) newNode(sizeof(V6Asof), T_CustomScanState);

	st->css.methods = &asof_exec_methods;
	st->css.slotOps = &TTSOpsMinimalTuple;
	return (Node *) st;
}

const CustomScanMethods sift_agg_v6_asof_plan_methods = {"SiftAggV6Asof", asof_create_state};
