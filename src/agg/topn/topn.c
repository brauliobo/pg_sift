/*
 * agg/topn/topn.c - the path and the node of SiftAggV6TopN: the query is recognized at UPPERREL_ORDERED, the keys and the clauses are checked against the index, and the node runs the
 * tournament (agg/topn/tourn.h) over the units the format opens, then gives the winners' columns in order.
 */
#include "postgres.h"

#include "access/genam.h"
#include "access/table.h"
#include "catalog/pg_type_d.h"
#include "commands/explain.h"
#include "common/int.h"
#include "executor/executor.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "optimizer/cost.h"
#include "optimizer/optimizer.h"
#include "optimizer/tlist.h"
#include "parser/parsetree.h"
#include "rewrite/rewriteManip.h"
#include "utils/builtins.h"
#include "utils/fmgroids.h"
#include "utils/lsyscache.h"
#include "utils/ruleutils.h"
#include "utils/rel.h"
#include "utils/tuplesort.h"
#include "agg/test_gucs.h"

#include "agg/agg.h"
#include "agg/safety.h"
#include "agg/source_rel.h"
#include "agg/cost/card.h"
#include "agg/gpu.h"
#include "agg/explain.h"
#include "agg/topn/topn.h"
#include "agg/settings.h"
#include "core/memtrack.h"


/* the key types whose int64 is in the order of the type: integers and dates and timestamps as they are, float8 by sift_f64_order_i64, a numeric of at most 18 digits as its scaled integer */
static bool
int_order_type(Oid typid)
{
	switch (typid)
	{
		case INT2OID:
		case INT4OID:
		case INT8OID:
		case DATEOID:
		case TIMESTAMPOID:
		case TIMESTAMPTZOID:
		case FLOAT8OID:
			return true;
		default:
			return false;
	}
}

/* the parameter of an initplan as the initplan that sets it: a plan that is not made yet has no ancestors to name the parameter by */
static Node *
initplan_mutator(Node *n, PlannerInfo *root)
{
	if (n != NULL && IsA(n, Param) && ((const Param *) n)->paramkind == PARAM_EXEC)
		foreach_node(SubPlan, sp, root->init_plans)
			if (list_member_int(sp->setParam, ((const Param *) n)->paramid))
				return copyObject((Node *) sp);
	return expression_tree_mutator(n, initplan_mutator, root);
}

/* a sort key of the table in hand as it is named in its own namespace; a column of another relation of the statement by its name */
static char *
sort_key_text(PlannerInfo *root, Index rti, List *dcx, Node *e)
{
	const Var  *v = (const Var *) e;

	if (IsA(e, Var) && v->varno != rti)
		return get_rte_attribute_name(planner_rt_fetch(v->varno, root), v->varattno);
	e = initplan_mutator(e, root);
	ChangeVarNodes(e, rti, 1, 0);
	return deparse_expression(e, dcx, false, false);
}

char *
sift_v6_topn_desc(PlannerInfo *root, Index rti)
{
	const Query *q = root->parse;
	const Oid	relid = planner_rt_fetch(rti, root)->relid;
	List	   *dcx = deparse_context_for(get_rel_name(relid), relid);
	StringInfoData buf;

	initStringInfo(&buf);
	foreach_node(SortGroupClause, sgc, q->sortClause)
	{
		bool		desc;

		get_equality_op_for_ordering_op(sgc->sortop, &desc);
		appendStringInfo(&buf, "%s%s%s", buf.len > 0 ? ", " : "", sort_key_text(root, rti, dcx, (Node *) get_sortgroupclause_expr(sgc, q->targetList)),
						 desc ? " DESC" : "");
		if (sgc->nulls_first != desc)
			appendStringInfoString(&buf, sgc->nulls_first ? " NULLS FIRST" : " NULLS LAST");
	}
	return buf.data;
}

char *
sift_v6_topn_limit_text(const Node *limit, const Node *offset)
{
	const Const *l = (const Const *) limit,
			   *o = (const Const *) offset;
	int64		sum;

	if (limit != NULL && IsA(limit, Const) && !l->constisnull && (offset == NULL || (IsA(offset, Const) && !o->constisnull)) &&
		!pg_add_s64_overflow(DatumGetInt64(l->constvalue), offset == NULL ? 0 : DatumGetInt64(o->constvalue), &sum))
		return psprintf(INT64_FORMAT, sum);
	return "by parameter";
}

void
sift_v6_topn_explain(ExplainState *es, const char *desc, const char *limit)
{
	ExplainPropertyText("Top-N", psprintf("%s (limit %s)", desc, limit), es);
}

/* the key types a sift_table orders as the rows of its table: the sort of its winners is the query's own, whose comparator and collation order the stored value exactly */
static bool
table_key_type_ok(Oid typid)
{
	switch (typid)
	{
		case UUIDOID:
		case BYTEAOID:
		case BPCHAROID:
		case NAMEOID:
		case NUMERICOID:
			return true;
		default:
			return int_order_type(typid);
	}
}

static bool
index_key_type_ok(const SiftV6InCol *c)
{
	return c->typid == NUMERICOID ? c->type == SVEC_NUM64 : int_order_type(c->typid);
}

/* The tournament orders the keys as int64 and a NULL: a numeric NaN, which sorts above every number, has neither, so a numeric key is sorted by the query's own comparator */
static bool
tournament_keys_ok(const SiftV6TopNSpec *spec)
{
	for (int k = 0; k < spec->nkeys; k++)
		if (spec->key[k].typid == NUMERICOID)
			return false;
	return !spec->is_table;
}

/* The external parameter a count is: bare, or under the implicit cast to int8 of an int2 or int4 one (PREPARE q(int) .. LIMIT $1) */
static const Param *
count_param(const Node *n)
{
	if (IsA(n, FuncExpr) && (((const FuncExpr *) n)->funcid == F_INT8_INT4 || ((const FuncExpr *) n)->funcid == F_INT8_INT2))
		n = linitial(((const FuncExpr *) n)->args);
	return IsA(n, Param) && ((const Param *) n)->paramkind == PARAM_EXTERN ? (const Param *) n : NULL;
}

/* A count the query gives as an int8 constant or an external parameter of an integer type (NULL: not given) */
static bool
count_ok(const Node *n)
{
	const Param *prm;

	if (n == NULL || IsA(n, Const))
		return n == NULL || ((const Const *) n)->consttype == INT8OID;
	prm = count_param(n);
	return prm != NULL && (prm->paramtype == INT8OID || prm->paramtype == INT4OID || prm->paramtype == INT2OID);
}

/* The value of a count; false: it is NULL (LIMIT ALL, no offset). A parameter has the value of this execution */
static bool
count_value(const ParamListInfo params, const Node *n, int64 *out)
{
	const Param *param;
	ParamExternData work;
	const ParamExternData *prm;

	if (n == NULL)
		return false;
	if (IsA(n, Const))
	{
		*out = ((const Const *) n)->constisnull ? 0 : DatumGetInt64(((const Const *) n)->constvalue);
		return !((const Const *) n)->constisnull;
	}
	param = count_param(n);
	prm = params->paramFetch != NULL ? params->paramFetch(params, param->paramid, false, &work) : &params->params[param->paramid - 1];
	*out = prm->isnull ? 0 : param->paramtype == INT8OID ? DatumGetInt64(prm->value) : param->paramtype == INT4OID ? DatumGetInt32(prm->value) : DatumGetInt16(prm->value);
	return !prm->isnull;
}

/* offset + limit for the tournament; false when a run must sort the rows it is given: LIMIT ALL, WITH TIES, a sum past SIFT_TOURN_MAX_K */
static bool
tournament_rows(const ParamListInfo params, const SiftV6TopNSpec *spec, uint64 *rows)
{
	int64		limit,
				offset = 0,
				sum;

	if (spec->with_ties || !count_value(params, spec->limit, &limit))
		return false;
	count_value(params, spec->offset, &offset);
	if (pg_add_s64_overflow(limit, offset, &sum) || sum > SIFT_TOURN_MAX_K)
		return false;
	*rows = (uint64) sum;
	return true;
}

/* offset + limit as the planner costs them: a parameter's value is not known, a typical page of results is */
#define SIFT_V6_TOPN_PARAM_ROWS	100

static double
plan_rows(const Node *limit, const Node *offset)
{
	const Const *l = (const Const *) limit,
			   *o = (const Const *) offset;

	return (IsA(limit, Const) && !l->constisnull ? (double) DatumGetInt64(l->constvalue) : SIFT_V6_TOPN_PARAM_ROWS) +
		(offset == NULL ? 0 : IsA(offset, Const) && !o->constisnull ? (double) DatumGetInt64(o->constvalue) : 0);
}

static bool
shape_ok(PlannerInfo *root, SiftV6TopNSpec *spec)
{
	const Query *q = root->parse;
	const Const *l = (const Const *) q->limitCount,
			   *o = (const Const *) q->limitOffset;

	if (!sift_enable_agg || q->commandType != CMD_SELECT || q->hasAggs || q->groupClause != NIL || q->groupingSets != NIL || q->hasWindowFuncs ||
		q->hasTargetSRFs || q->distinctClause != NIL || q->setOperations != NULL || q->rowMarks != NIL || q->sortClause == NIL ||
		list_length(q->sortClause) > SIFT_TOURN_MAX_KEYS || q->limitCount == NULL || !count_ok(q->limitCount) || !count_ok(q->limitOffset))
		return false;
	if (IsA(l, Const) && (l->constisnull || DatumGetInt64(l->constvalue) <= 0))
		return false;
	if (q->limitOffset != NULL && IsA(o, Const) && !o->constisnull && DatumGetInt64(o->constvalue) < 0)
		return false;
	spec->limit = q->limitCount;
	spec->offset = q->limitOffset;
	spec->with_ties = q->limitOption == LIMIT_OPTION_WITH_TIES;
	return q->limitOption == LIMIT_OPTION_COUNT || spec->with_ties;
}

typedef struct TopNPlan
{
	PlannerInfo *root;
	RelOptInfo *rel;
	RelOptInfo *out;
	const SiftV6RelSource *src;
	const IndexOptInfo *index;	/* NULL for a sift_table */
	SiftV6TopNSpec spec;
	SiftV6Input in;
	List	   *scan_tlist;
	List	   *quals;
} TopNPlan;

/* the key of a Var: of a table the column to fetch, of an index the column of the input the tournament reads */
static bool
key_col(TopNPlan *p, const Var *v, SiftV6Col *key)
{
	int			slot;

	if (p->spec.is_table)
	{
		*key = (SiftV6Col) {.attno = v->varattno,.typid = v->vartype};
		return table_key_type_ok(v->vartype);
	}
	if ((slot = sift_v6_input_add_var(&p->in, v)) < 0 || !index_key_type_ok(&p->in.col[slot]))
		return false;
	*key = (SiftV6Col) {.attno = p->in.col[slot].attno,.typid = p->in.col[slot].typid,.scale = p->in.col[slot].scale};
	return true;
}

static bool
compile_keys(TopNPlan *p)
{
	const Query *q = p->root->parse;

	foreach_node(SortGroupClause, sgc, q->sortClause)
	{
		const Node *e = (Node *) get_sortgroupclause_expr(sgc, q->targetList);
		SiftV6TopNSpec *s = &p->spec;
		bool		reverse;

		if (!IsA(e, Var) || ((Var *) e)->varno != p->rel->relid || !OidIsValid(get_equality_op_for_ordering_op(sgc->sortop, &reverse)) ||
			!key_col(p, (Var *) e, &s->key[s->nkeys]))
			return false;
		s->order[s->nkeys] = (SiftTournKey) {.desc = reverse,.nulls_first = sgc->nulls_first};
		s->sortop[s->nkeys] = sgc->sortop;
		s->collation[s->nkeys] = exprCollation(e);
		s->nkeys++;
	}
	return true;
}

/* The place (0-based) in the tuples the node outputs of the column of the index whose number is col (1-based); -1: it is not one of them */
static int
tlist_pos(const List *tlist, const SiftV6Input *in, int col)
{
	int			i = 0;

	foreach_node(TargetEntry, te, (List *) tlist)
	{
		if (((Var *) te->expr)->varattno == sift_v6_input_attno(in, col))
			return i;
		i++;
	}
	return -1;
}

/* the columns the node outputs are those of the table, read by the source from wherever it holds them: any plain column of the relation */
static bool
compile_outputs(TopNPlan *p)
{
	p->scan_tlist = sift_v6_scan_tlist(p->out->reltarget->exprs, p->rel->relid, T_Invalid);
	foreach_node(TargetEntry, te, p->scan_tlist)
		if (((Var *) te->expr)->varattno <= 0)
			return false;
	for (int k = 0; k < p->spec.nkeys; k++)
		if (tlist_pos(p->scan_tlist, &p->in, p->spec.key[k].attno) < 0)
			return false;
	return p->scan_tlist != NIL;
}

static bool
compile_quals(TopNPlan *p)
{
	SiftV6Vexpr probe;

	foreach_node(RestrictInfo, ri, p->rel->baserestrictinfo)
	{
		if (ri->pseudoconstant)
			return false;
		p->quals = lappend(p->quals, ri->clause);
	}
	return sift_v6_vexpr_quals(CurrentMemoryContext, p->quals, p->rel->relid, NULL, &p->in, &probe);
}

static const CustomPathMethods topn_path_methods = {"SiftAggV6TopN", sift_agg_v6_topn_plan_path, NULL};

/* Whether the leading key reads in the order of the parts, or in the reverse of it (NULLs at the other end): the tournament then reads from the best granule on and stops where the rows are enough */
static bool
leading_key_ordered(const SiftV6TopNSpec *spec)
{
	const int	att = spec->key[0].attno;

	return !spec->is_table && sift_v6_rel_sorted_by(spec->indexoid, false, &att, 1) && spec->order[0].desc == spec->order[0].nulls_first;
}

static double
index_total(const TopNPlan *p, double k)
{
	const double sel = Max(p->rel->rows / Max(p->rel->tuples, 1.0), 1e-9);
	const bool	ordered = leading_key_ordered(&p->spec);
	const double read = ordered ? Min(p->rel->tuples, k / sel + 4.0 * SIFT_VEC_LEN) : p->rel->tuples;
	Cost		open,
				scan;

	sift_v6_scan_price(p->root, p->rel, p->index, 0, &open, &scan);
	if (k > SIFT_TOURN_MAX_K)
		return scan + cpu_tuple_cost * p->rel->rows + cpu_operator_cost * p->rel->rows * (p->spec.nkeys + list_length(p->quals) + log2(Max(p->rel->rows, 2.0)));
	if (!ordered && sift_v6_gpu_topn_forced(&p->spec))
		return open + cpu_tuple_cost * k;
	return open + (scan - open) * read / Max(p->rel->tuples, 1.0) + cpu_tuple_cost * k + cpu_operator_cost * read * (p->spec.nkeys + list_length(p->quals));
}

/* A scan with no tournament (a sift_table, a numeric key): its columns are read whole in vectors, and each row that passes the quals is fetched once into a sort bounded by the limit */
static double
rows_total(const TopNPlan *p, double k)
{
	Cost		open,
				scan;

	sift_v6_source_scan_price(p->root, p->rel, p->src, 0, &open, &scan);
	return scan + cpu_tuple_cost * k + sift_v6_fetch_cost(p->spec.is_table) * p->rel->rows +
		cpu_operator_cost * (p->rel->tuples * list_length(p->quals) + p->rel->rows * p->spec.nkeys);
}

static void
topn_add_path(TopNPlan *p)
{
	const double k = plan_rows(p->spec.limit, p->spec.offset);
	const double total = tournament_keys_ok(&p->spec) ? index_total(p, k) : rows_total(p, k);

	sift_v6_add_path(p->out, &topn_path_methods, total - cpu_tuple_cost * k, total, Min(k, p->rel->rows), p->root->sort_pathkeys,
					 list_make3(sift_v6_topn_spec_pack(&p->spec), p->scan_tlist, p->quals), NIL);
}

void
sift_agg_v6_topn_upper_paths(PlannerInfo *root, UpperRelationKind stage, RelOptInfo *input_rel, RelOptInfo *output_rel, void *extra)
{
	SiftV6TopNSpec shape = {0};
	SiftV6RelSource *src;
	int			nsrc;

	if (stage != UPPERREL_ORDERED || !sift_v6_base_rel_ok(root, input_rel) || !shape_ok(root, &shape))
		return;
	sift_v6_stage_target(root, stage, output_rel);
	src = sift_v6_rel_sources_alloc(root, input_rel, &nsrc);
	for (int i = 0; i < nsrc; i++)
	{
		TopNPlan	p = {.root = root,.rel = input_rel,.out = output_rel,.src = &src[i],.index = src[i].ix,.spec = shape,.in = src[i].input};

		if (!sift_v6_source_usable(&src[i]) || (!src[i].is_table && !sift_v6_rel_ordered(src[i].reloid, false)))
			continue;
		p.spec.indexoid = src[i].reloid;
		p.spec.is_table = src[i].is_table;
		p.spec.rti = input_rel->relid;
		if (compile_keys(&p) && compile_outputs(&p) && compile_quals(&p))
		{
			p.spec.desc = sift_v6_topn_desc(root, input_rel->relid);
			topn_add_path(&p);
			return;
		}
	}
}

Plan *
sift_agg_v6_topn_plan_path(PlannerInfo *root, RelOptInfo *rel, CustomPath *best, List *tlist, List *clauses, List *custom_plans)
{
	const SiftV6TopNSpec *spec = sift_v6_topn_spec_unpack(linitial(best->custom_private));

	return sift_v6_plan_scan(root, best, tlist, &sift_agg_v6_topn_plan_methods, spec->indexoid, (Node *) list_make3(spec->limit, spec->offset, lthird(best->custom_private)));
}

List *
sift_v6_topn_spec_pack(const SiftV6TopNSpec *s)
{
	List	   *l = list_make4_int((int) s->indexoid, (int) s->rti, s->nkeys, s->is_table);

	for (int i = 0; i < s->nkeys; i++)
	{
		l = lappend_int(lappend_int(l, s->key[i].attno), (int) s->key[i].typid);
		l = lappend_int(lappend_int(l, s->key[i].scale), s->order[i].desc | (s->order[i].nulls_first << 1));
		l = lappend_int(lappend_int(l, (int) s->sortop[i]), (int) s->collation[i]);
	}
	return list_make2(l, list_make4(s->limit, s->offset, makeBoolConst(s->with_ties, false), makeString(s->desc)));
}

SiftV6TopNSpec *
sift_v6_topn_spec_unpack(const List *l)
{
	const List *ints = linitial(l);
	const List *rest = lsecond(l);
	SiftV6TopNSpec *s = palloc0(sizeof(SiftV6TopNSpec));

	s->indexoid = (Oid) list_nth_int(ints, 0);
	s->rti = (Index) list_nth_int(ints, 1);
	s->nkeys = list_nth_int(ints, 2);
	s->is_table = list_nth_int(ints, 3);
	for (int i = 0; i < s->nkeys; i++)
	{
		const int	at = 4 + 6 * i;

		s->key[i] = (SiftV6Col) {.attno = list_nth_int(ints, at),.typid = (Oid) list_nth_int(ints, at + 1),.scale = list_nth_int(ints, at + 2)};
		s->order[i] = (SiftTournKey) {.desc = list_nth_int(ints, at + 3) & 1,.nulls_first = (list_nth_int(ints, at + 3) >> 1) & 1};
		s->sortop[i] = (Oid) list_nth_int(ints, at + 4);
		s->collation[i] = (Oid) list_nth_int(ints, at + 5);
	}
	s->limit = linitial(rest);
	s->offset = lsecond(rest);
	s->with_ties = DatumGetBool(((Const *) lthird(rest))->constvalue);
	s->desc = strVal(lfourth(rest));
	return s;
}

const SiftTopNWinRow *
sift_v6_topn_run(MemoryContext cxt, const SiftV6TopNSpec *spec, SiftV6TopNSource *src, SiftTournament **tp, uint32 *n)
{
	SiftTournSpec ts = {.nkeys = (uint8) spec->nkeys,.offset = 0,.limit = spec->rows,.with_ties = spec->with_ties};

	for (int i = 0; i < spec->nkeys; i++)
		ts.keys[i] = spec->order[i];
	*tp = sift_tourn_create(cxt, &ts, (uint64) work_mem * 1024 / 4, NULL);
	src->seed(src->src, *tp);
	sift_tourn_run(*tp, &src->ops, src->src);
	return sift_tourn_result(*tp, n);
}

typedef struct TopNState
{
	CustomScanState css;
	SiftV6TopNSpec *spec;
	AttrNumber *attnos;
	int			nattrs;
	List	   *quals;
	MemoryContext cx;
	SiftV6TopNSource src;
	SiftTournament *tourn;
	const SiftTopNWinRow *rows;
	uint32		nrows;
	uint32		pos;
	SiftV6Gpu	gpu;
	bool		ran;
	Tuplesortstate *sort;		/* the run that sorted its rows */
	TupleTableSlot *sorted;
} TopNState;

static void
topn_begin(CustomScanState *node, EState *estate, int eflags)
{
	TopNState  *st = (TopNState *) node;
	const CustomScan *cs = (CustomScan *) node->ss.ps.plan;
	List	   *tlist = lsecond(cs->custom_private);
	sift_v6_test_wait();
	int			i = 0;

	st->spec = sift_v6_topn_spec_unpack(linitial(cs->custom_private));
	st->quals = lthird(cs->custom_private);
	st->nattrs = list_length(tlist);
	st->attnos = palloc(st->nattrs * sizeof(AttrNumber));
	foreach_node(TargetEntry, te, tlist)
		st->attnos[i++] = ((Var *) te->expr)->varattno;
}

static void
topn_release(TopNState *st)
{
	if (st->tourn != NULL)
		sift_tourn_free(st->tourn);
	if (st->sort != NULL)
	{
		tuplesort_end(st->sort);
		ExecDropSingleTupleTableSlot(st->sorted);
	}
	st->sort = NULL;
	st->sorted = NULL;
	sift_v6_gpu_end(&st->gpu);
	st->gpu = (SiftV6Gpu) {0};
	if (st->src.close != NULL)
		st->src.close(st->src.src);
	if (st->cx != NULL)
		MemoryContextDelete(st->cx);
	st->tourn = NULL;
	st->src = (SiftV6TopNSource) {0};
	st->cx = NULL;
	st->ran = false;
	st->rows = NULL;
}

/* The rows the source gives (opened unsorted: the tuplesort orders them), each with the columns the node outputs, kept to the first offset + limit in order */
static void
topn_sort_rows(TopNState *st, EState *estate, const SiftV6Input *in, const SiftV6Vexpr *quals)
{
	const SiftV6TopNSpec *spec = st->spec;
	TupleDesc	desc = st->css.ss.ss_ScanTupleSlot->tts_tupleDescriptor;
	TupleTableSlot *slot = st->css.ss.ss_ScanTupleSlot;
	SiftV6Source *src = sift_v6_rel_open(estate, in, quals, false);
	List	   *tlist = lsecond(((CustomScan *) st->css.ss.ps.plan)->custom_private);
	AttrNumber	keycols[SIFT_TOURN_MAX_KEYS];
	bool		nullsfirst[SIFT_TOURN_MAX_KEYS];
	SiftV6Batch b;
	int64		limit,
				offset = 0,
				bound;

	for (int k = 0; k < spec->nkeys; k++)
	{
		keycols[k] = (AttrNumber) (tlist_pos(tlist, in, spec->key[k].attno) + 1);
		nullsfirst[k] = spec->order[k].nulls_first;
	}
	st->sort = tuplesort_begin_heap(desc, spec->nkeys, keycols, (Oid *) spec->sortop, (Oid *) spec->collation, nullsfirst, work_mem, NULL, TUPLESORT_ALLOWBOUNDED);
	count_value(estate->es_param_list_info, spec->offset, &offset);
	if (!spec->with_ties && count_value(estate->es_param_list_info, spec->limit, &limit) && !pg_add_s64_overflow(limit, offset, &bound))
		tuplesort_set_bound(st->sort, bound);
	PG_TRY();
	{
		while (src->next(src, &b))
		{
			CHECK_FOR_INTERRUPTS();
			for (int w = 0; w < (b.n + 63) / 64; w++)
				for (uint64 bits = b.mask != NULL ? b.mask[w] : ~UINT64CONST(0); bits != 0; bits &= bits - 1)
				{
					const int	r = w * 64 + __builtin_ctzll(bits);

					if (r >= b.n)
						break;
					ExecClearTuple(slot);
					if (!src->fetch(src, b.tag[r], st->attnos, st->nattrs, slot->tts_values, slot->tts_isnull))
						continue;
					ExecStoreVirtualTuple(slot);
					tuplesort_puttupleslot(st->sort, slot);
				}
		}
	}
	PG_FINALLY();
	{
		src->close(src);
	}
	PG_END_TRY();
	tuplesort_performsort(st->sort);
	st->sorted = MakeSingleTupleTableSlot(desc, &TTSOpsMinimalTuple);
}

/* ORDER BY one key LIMIT n that the order of the parts cannot answer early: the device takes the best rows of the parts it reads, and the tournament the rest */
static void
topn_device(TopNState *st, EState *estate, Relation index, SiftV6Input *in)
{
	SiftV6TopNDevice dev;
	Relation	heap;
	bool		taken;

	if (st->src.device == NULL || st->spec->nkeys != 1)
		return;
	heap = table_open(index->rd_index->indrelid, AccessShareLock);
	taken = sift_v6_gpu_topn(st->spec, in, st->quals, heap, index, estate->es_snapshot, leading_key_ordered(st->spec), st->cx, &st->gpu, &dev);
	table_close(heap, NoLock);
	if (taken)
		st->src.device(st->src.src, &dev);
}

static void
topn_run(TopNState *st, EState *estate)
{
	SiftV6Input in;
	SiftV6Vexpr quals;
	bool		compiled,
				opened;
	bool		(*topn_open) (EState *estate, const SiftV6TopNSpec *spec, const SiftV6Input *in, const SiftV6Vexpr *quals, SiftV6TopNSource *dst);
	Relation	source = sift_v6_source_open(st->spec->indexoid, st->spec->is_table, AccessShareLock, &in);

	sift_v6_source_enter(source, st->spec->is_table, estate->es_snapshot);
	st->cx = sift_mem_allocset_create(estate->es_query_cxt, "sift agg v6 topn");
	sift_v6_input_bind(&in, estate);
	compiled = sift_v6_vexpr_quals(st->cx, st->quals, st->spec->rti, estate->es_param_list_info, &in, &quals);
	SIFT_INVARIANT(compiled, "the quals of the Top-N scan of relation %u are not compiled", st->spec->indexoid);
	st->pos = 0;
	st->ran = true;
	topn_open = tournament_keys_ok(st->spec) ? sift_store_ops_for_oid(st->spec->indexoid, false)->order->topn_open : NULL;
	opened = topn_open != NULL && tournament_rows(estate->es_param_list_info, st->spec, &st->spec->rows) &&
		topn_open(estate, st->spec, &in, st->quals == NIL ? NULL : &quals, &st->src);
	if (!opened)
	{
		sift_v6_source_close(source, st->spec->is_table, NoLock);
		topn_sort_rows(st, estate, &in, st->quals == NIL ? NULL : &quals);
		return;
	}
	topn_device(st, estate, source, &in);
	sift_v6_source_close(source, st->spec->is_table, NoLock);
	st->rows = sift_v6_topn_run(st->cx, st->spec, &st->src, &st->tourn, &st->nrows);
}

static TupleTableSlot *
topn_next(ScanState *ss)
{
	TopNState  *st = (TopNState *) ss;
	TupleTableSlot *slot = ss->ss_ScanTupleSlot;
	bool		fetched;

	if (!st->ran)
		topn_run(st, ss->ps.state);
	ExecClearTuple(slot);
	if (st->sort != NULL)
		return tuplesort_gettupleslot(st->sort, true, false, st->sorted, NULL) ? ExecCopySlot(slot, st->sorted) : NULL;
	if (st->pos == st->nrows)
		return NULL;
	fetched = st->src.fetch(st->src.src, &st->rows[st->pos++], st->attnos, st->nattrs, slot->tts_values, slot->tts_isnull);
	SIFT_INVARIANT(fetched, "a winner of the Top-N scan of index %u has no row", st->spec->indexoid);
	return ExecStoreVirtualTuple(slot);
}

static bool
topn_recheck(ScanState *ss, TupleTableSlot *slot)
{
	return true;
}

static TupleTableSlot *
topn_exec(CustomScanState *node)
{
	return ExecScan(&node->ss, topn_next, topn_recheck);
}

static void
topn_end(CustomScanState *node)
{
	topn_release((TopNState *) node);
}

/* a scan again with the same parameters gives the same rows: the winners are kept; other parameters make another tournament */
static void
topn_rescan(CustomScanState *node)
{
	TopNState  *st = (TopNState *) node;

	if (node->ss.ps.chgParam != NULL)
		topn_release(st);
	st->pos = 0;
	if (st->sort != NULL)
		tuplesort_rescan(st->sort);
}

static void
topn_explain(CustomScanState *node, List *ancestors, ExplainState *es)
{
	TopNState  *st = (TopNState *) node;

	sift_v6_explain_index(es, st->spec->indexoid, st->spec->is_table);
	sift_v6_topn_explain(es, st->spec->desc, sift_v6_topn_limit_text(st->spec->limit, st->spec->offset));
	sift_v6_gpu_explain(&st->gpu, es);
	if (st->tourn != NULL)
		sift_v6_explain_count(es, "Top-N Rows", (int64) st->spec->rows);
	if (st->tourn != NULL)
	{
		const SiftTournStats *ts = sift_tourn_stats(st->tourn);
		const SiftV6SkipStats none = {0},
				   *skips = st->src.skips != NULL ? st->src.skips : &none;

		sift_v6_explain_count(es, "Partitions Skipped", (int64) (ts->partitions_pruned + skips->partitions));
		sift_v6_explain_count(es, "Parts Skipped", (int64) (ts->parts_pruned + skips->parts));
		sift_v6_explain_count(es, "Granules Scanned", (int64) ts->granules_scanned);
		sift_v6_explain_count(es, "Granules Pruned", (int64) (ts->granules_pruned + skips->granules));
	}
}

static const CustomExecMethods topn_exec_methods = {
	.CustomName = "SiftAggV6TopN",
	.BeginCustomScan = topn_begin,
	.ExecCustomScan = topn_exec,
	.EndCustomScan = topn_end,
	.ReScanCustomScan = topn_rescan,
	.ExplainCustomScan = topn_explain,
};

static Node *
topn_create_state(CustomScan *cscan)
{
	TopNState  *st = (TopNState *) newNode(sizeof(TopNState), T_CustomScanState);

	st->css.methods = &topn_exec_methods;
	return (Node *) st;
}

const CustomScanMethods sift_agg_v6_topn_plan_methods = {"SiftAggV6TopN", topn_create_state};
