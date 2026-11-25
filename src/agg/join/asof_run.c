/*
 * agg/join/asof_run.c - the ASOF join of SiftAggV6: the path of PostgreSQL's idiom
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
#include "catalog/pg_inherits.h"
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
#include "utils/builtins.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"
#include "utils/tuplesort.h"
#include "utils/typcache.h"

#include "agg/agg.h"
#include "agg/safety.h"
#include "agg/join/asof.h"
#include "agg/batch.h"
#include "agg/join/join.h"
#include "agg/plan/shape.h"
#include "agg/source_rel.h"
#include "agg/vexpr.h"
#include "agg/settings.h"

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
	id->shape.single_table = sift_v6_rte_stored(rte) && !has_subclass(rte->relid) && rte->tablesample == NULL;
	limit = (const Const *) eval_const_expressions(NULL, sub->limitCount);
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
	return sift_v6_rel_sorted_by(in->indexoid, in->is_table, atts, 2);
}

static bool
sorted_table(const RangeTblEntry *rte, const Idiom *id)
{
	SiftV6Input in;
	Relation	tbl;

	if (!sift_v6_rel_ordered(rte->relid, true))
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

	if (ix->hypothetical || ix->indpred != NIL || !sift_v6_rel_ordered(ix->indexoid, false))
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

/* One merge of the fact rows with dim: the rows must come in the order of (k, ts) */
typedef struct Merge
{
	DimFeed    *feed;
	SiftAsofStream *stream;
} Merge;

/* Where the node is: the fact rows that arrive in order of (k, ts) are merged as they come, the rest (sorted) after them */
typedef enum AsofPhase
{
	ASOF_IDLE,
	ASOF_IN_ORDER,
	ASOF_SORTED,
	ASOF_DONE
} AsofPhase;

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
	bool		have_prev;		/* the last fact row of the run that arrived in order: its key and time */
	int64		prev_k,
				prev_ts;
	bool		prev_knull,
				prev_tsnull;
	MemoryContext cx;			/* one run: the merges and the rows of their streams */
	MemoryContext row_cx;		/* what one row of the join takes */
	AsofPhase	phase;
	Merge		merge;
	Tuplesortstate *rest;		/* the fact rows that did not arrive in order */
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

/* the sort of the fact rows that do not arrive in the order of (k, ts), the NULLs last */
static Tuplesortstate *
sort_begin(V6Asof *st)
{
	const AttrNumber cols[2] = {st->fact_k, st->fact_ts};
	const Oid	typs[2] = {st->ktyp, st->tstyp};
	Oid			ops[2];
	Oid			colls[2] = {InvalidOid, InvalidOid};
	bool		nulls_first[2] = {false, false};

	for (int i = 0; i < 2; i++)
	{
		ops[i] = lookup_type_cache(typs[i], TYPECACHE_LT_OPR)->lt_opr;
		if (!OidIsValid(ops[i]))
			ereport(ERROR, (errcode(ERRCODE_UNDEFINED_FUNCTION), errmsg("could not identify a less-than operator for type %s", format_type_be(typs[i]))));
	}
	return tuplesort_begin_heap(ExecGetResultType(st->fps), 2, (AttrNumber *) cols, ops, colls, nulls_first, work_mem, NULL, TUPLESORT_NONE);
}

/* the order of (k, ts) with the NULLs last: a < b, a = b or a > b as -1, 0, 1 */
static int
fact_cmp(int64 ak, bool aknull, int64 ats, bool atsnull, int64 bk, bool bknull, int64 bts, bool btsnull)
{
	if (aknull != bknull)
		return aknull ? 1 : -1;
	if (!aknull && ak != bk)
		return ak < bk ? -1 : 1;
	if (atsnull != btsnull)
		return atsnull ? 1 : -1;
	return atsnull || ats == bts ? 0 : ats < bts ? -1 : 1;
}

/* The key and time of a fact row, and whether the row follows the last one that arrived in order (it then joins the run); the first row that does not ends the run */
static bool
fact_in_order(V6Asof *st, TupleTableSlot *slot)
{
	const bool	knull = slot->tts_isnull[st->fact_k - 1],
				tsnull = slot->tts_isnull[st->fact_ts - 1];
	const int64 k = knull ? 0 : int_datum(st->ktyp, slot->tts_values[st->fact_k - 1]),
				ts = tsnull ? 0 : int_datum(st->tstyp, slot->tts_values[st->fact_ts - 1]);

	if (st->have_prev && fact_cmp(st->prev_k, st->prev_knull, st->prev_ts, st->prev_tsnull, k, knull, ts, tsnull) > 0)
		return false;
	st->have_prev = true;
	st->prev_k = k;
	st->prev_knull = knull;
	st->prev_ts = ts;
	st->prev_tsnull = tsnull;
	return true;
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


static Merge
merge_open(V6Asof *st, EState *estate)
{
	SiftV6Input in;
	Var		   *kvar = makeVar(1, st->dim_k, st->ktyp, -1, InvalidOid, 0);
	Var		   *tvar = makeVar(1, st->dim_ts, st->tstyp, -1, InvalidOid, 0);
	Merge		m = {.feed = MemoryContextAllocZero(st->cx, sizeof(DimFeed))};

	dim_input(st, &in);
	m.feed->kslot = sift_v6_input_add_var(&in, kvar);
	m.feed->tslot = sift_v6_input_add_var(&in, tvar);
	SIFT_INVARIANT(m.feed->kslot >= 0 && m.feed->tslot >= 0, "the asof join of relation %u does not read its keys", st->indexoid);
	m.feed->src = sift_v6_rel_open(estate, &in, NULL, true);
	m.stream = sift_asof_open(st->cx, st->spec, dim_next, m.feed);
	return m;
}

static void
merge_close(V6Asof *st, Merge *m)
{
	const SiftAsofStats *s = sift_asof_stats(m->stream);

	st->stats.fact_rows += s->fact_rows;
	st->stats.dim_rows += s->dim_rows;
	st->stats.dim_null_rows += s->dim_null_rows;
	st->stats.matched += s->matched;
	sift_asof_close(m->stream);
	m->feed->src->close(m->feed->src);
}

/* The join row of one fact row, in the scan slot: the match in dim and the columns it gives; false when an inner join has none */
static bool
merge_row(V6Asof *st, Merge *m, TupleTableSlot *slot)
{
	TupleTableSlot *out = st->css.ss.ss_ScanTupleSlot;
	MemoryContext old;
	bool		fetched = false;
	SiftAsofHit hit;

	slot_getallattrs(slot);
	hit = sift_asof_probe(m->stream, slot->tts_isnull[st->fact_k - 1] ? 0 : int_datum(st->ktyp, slot->tts_values[st->fact_k - 1]), slot->tts_isnull[st->fact_k - 1],
						  slot->tts_isnull[st->fact_ts - 1] ? 0 : int_datum(st->tstyp, slot->tts_values[st->fact_ts - 1]), slot->tts_isnull[st->fact_ts - 1]);
	CHECK_FOR_INTERRUPTS();
	if (!hit.found && !st->left)
		return false;
	MemoryContextReset(st->row_cx);
	old = MemoryContextSwitchTo(st->row_cx);
	if (hit.found && st->nfetch > 0)
		fetched = m->feed->src->fetch(m->feed->src, hit.tag, st->fetch, st->nfetch, st->dvals, st->dnulls);
	MemoryContextSwitchTo(old);
	ExecClearTuple(out);
	for (int i = 0; i < st->nout; i++)
	{
		if (!st->dim[i])
		{
			out->tts_values[i] = slot->tts_values[st->col[i] - 1];
			out->tts_isnull[i] = slot->tts_isnull[st->col[i] - 1];
		}
		else
		{
			out->tts_isnull[i] = !fetched || st->dnulls[st->fetch_at[i]];
			out->tts_values[i] = out->tts_isnull[i] ? (Datum) 0 : st->dvals[st->fetch_at[i]];
		}
	}
	return ExecStoreVirtualTuple(out) != NULL;
}

static void
asof_start(V6Asof *st, EState *estate)
{
	st->cx = sift_mem_allocset_create(estate->es_query_cxt, "sift agg v6 run");
	st->row_cx = AllocSetContextCreate(st->cx, "sift agg v6 tuple", ALLOCSET_SMALL_SIZES);
	st->stats = (SiftAsofStats) {0};
	st->have_prev = false;
	st->merge = merge_open(st, estate);
	st->phase = ASOF_IN_ORDER;
}

/* the sort of the rows that did not arrive in order is done: they are merged against dim read again */
static void
asof_sorted(V6Asof *st, EState *estate)
{
	merge_close(st, &st->merge);
	st->merge = (Merge) {0};
	if (st->rest == NULL)
	{
		st->phase = ASOF_DONE;
		return;
	}
	tuplesort_performsort(st->rest);
	st->merge = merge_open(st, estate);
	st->phase = ASOF_SORTED;
}

/*
 * The fact rows that arrive in the order of (k, ts) are merged as they come, with no sort, and each gives its row to the caller at once; the first row that does not ends that run, and it and
 * every row after it are sorted and merged against dim read again (a second merge: the answer of a fact row does not depend on the others, only the order the merge reads them in does).
 */
static TupleTableSlot *
asof_next(ScanState *ss)
{
	V6Asof	   *st = (V6Asof *) ss;
	EState	   *estate = ss->ps.state;

	if (st->phase == ASOF_IDLE)
		asof_start(st, estate);
	while (st->phase == ASOF_IN_ORDER)
	{
		TupleTableSlot *slot = ExecProcNode(st->fps);

		if (TupIsNull(slot))
		{
			asof_sorted(st, estate);
			break;
		}
		CHECK_FOR_INTERRUPTS();
		slot_getallattrs(slot);
		if (st->rest == NULL && fact_in_order(st, slot))
		{
			if (merge_row(st, &st->merge, slot))
				return ss->ss_ScanTupleSlot;
			continue;
		}
		if (st->rest == NULL)
			st->rest = sort_begin(st);
		tuplesort_puttupleslot(st->rest, slot);
	}
	while (st->phase == ASOF_SORTED)
	{
		if (!tuplesort_gettupleslot(st->rest, true, false, st->fslot, NULL))
		{
			merge_close(st, &st->merge);
			st->merge = (Merge) {0};
			st->phase = ASOF_DONE;
			break;
		}
		if (merge_row(st, &st->merge, st->fslot))
			return ss->ss_ScanTupleSlot;
	}
	return NULL;
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
asof_release(V6Asof *st)
{
	if (st->merge.stream != NULL)
		merge_close(st, &st->merge);
	if (st->rest != NULL)
		tuplesort_end(st->rest);
	if (st->cx != NULL)
		MemoryContextDelete(st->cx);
	st->merge = (Merge) {0};
	st->rest = NULL;
	st->cx = st->row_cx = NULL;
	st->phase = ASOF_IDLE;
}

static void
asof_end(CustomScanState *node)
{
	V6Asof	   *st = (V6Asof *) node;

	asof_release(st);
	ExecDropSingleTupleTableSlot(st->fslot);
	ExecEndNode(st->fps);
}

/* a scan again runs the join again over the fact rows scanned again: the rows are given as they are made and are not kept */
static void
asof_rescan(CustomScanState *node)
{
	V6Asof	   *st = (V6Asof *) node;

	asof_release(st);
	if (node->ss.ps.chgParam != NULL)
		UpdateChangedParamSet(st->fps, node->ss.ps.chgParam);
	if (st->fps->chgParam == NULL)
		ExecReScan(st->fps);
}

static void
asof_explain(CustomScanState *node, List *ancestors, ExplainState *es)
{
	V6Asof	   *st = (V6Asof *) node;
	SiftAsofStats s = st->stats;

	ExplainPropertyText(st->is_table ? "Table" : "Index", get_rel_name(st->indexoid), es);
	ExplainPropertyText("Direction", st->spec.dir == SIFT_ASOF_BACKWARD ? (st->spec.strict ? "Backward, strict" : "Backward") : (st->spec.strict ? "Forward, strict" : "Forward"), es);
	if (st->merge.stream != NULL)
	{
		const SiftAsofStats *live = sift_asof_stats(st->merge.stream);

		s.fact_rows += live->fact_rows;
		s.dim_rows += live->dim_rows;
		s.matched += live->matched;
	}
	if (st->phase != ASOF_IDLE || s.fact_rows > 0)
	{
		sift_v6_explain_count(es, "Fact Rows", (int64) s.fact_rows);
		sift_v6_explain_count(es, "Dim Rows Read", (int64) s.dim_rows);
		sift_v6_explain_count(es, "Matched", (int64) s.matched);
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
	st->css.slotOps = &TTSOpsVirtual;
	return (Node *) st;
}

const CustomScanMethods sift_agg_v6_asof_plan_methods = {"SiftAggV6Asof", asof_create_state};
