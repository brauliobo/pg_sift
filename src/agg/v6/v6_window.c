/*
 * agg/v6/v6_window.c - the path and the node of SiftAggV6Window. The plan's private list holds what the node needs to describe itself: sift_v6_window_spec turns it into the spec of the
 * engine (agg/window.h) and the columns of the input; the planner calls it to know that the engine runs the query, the node to run it.
 */
#include "postgres.h"

#include <stdalign.h>

#include "access/genam.h"
#include "catalog/pg_collation_d.h"
#include "catalog/pg_namespace_d.h"
#include "catalog/pg_type_d.h"
#include "executor/executor.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "optimizer/cost.h"
#include "optimizer/optimizer.h"
#include "parser/parsetree.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/rel.h"
#include "utils/selfuncs.h"
#include "utils/tuplestore.h"

#include "agg/agg.h"
#include "agg/plan/safety.h"
#include "agg/v6/v6_card.h"
#include "agg/v6/v6_window.h"
#include "agg/v6/v6_win_carry.h"
#include "agg/v6/v6_win_par.h"
#include "agg/v6/v6_win_partsort.h"
#include "agg/v6/v6_winx_glue.h"

enum
{
	PRIV_INTS = 0,				/* indexoid, rti, mode, run condition function (-1: none), range frame, how the partitions are made (WIN_EXACT, WIN_PARTSORT, WIN_REGROUP), then per order key desc | nulls_first << 1 */
	PRIV_TLIST = 1,				/* the scan tlist: sift_v6_plan_scan reads it from here */
	PRIV_PART = 2,
	PRIV_ORD = 3,
	PRIV_FUNCS = 4,
	PRIV_BOUND = 5				/* the run condition bound: (a Const int8 or an external Param, a Const 1 when the condition is strict) or NIL */
};

/* How the rows of a partition reach the engine: the parts are sorted by the keys and the order, by the keys only (each partition is sorted), or by neither (the partitions are put together, then sorted) */
typedef enum WinPartMode
{
	WIN_EXACT,
	WIN_PARTSORT,
	WIN_REGROUP
} WinPartMode;

static bool
is_key_type(const SiftV6InCol *c)
{
	return c->type == SVEC_I32 || c->type == SVEC_I64;
}

static bool
is_arg_type(const SiftV6InCol *c)
{
	return c->type == SVEC_I32 || c->type == SVEC_I64 || c->type == SVEC_F64;
}

static int
key_slot(SiftV6Input *in, const Var *v, bool partsort)
{
	const int	text = partsort ? sift_v6_input_add_text(in, v) : -1;

	return text >= 0 ? text : sift_v6_input_add_var(in, v);
}

static bool
add_keys(SiftV6Input *in, const List *vars, int *slots, uint8 *n, bool partsort)
{
	*n = 0;
	if (list_length(vars) > SIFT_WIN_MAX_KEYS)
		return false;
	foreach_ptr(Var, v, (List *) vars)
	{
		const int	slot = key_slot(in, v, partsort);

		if (slot < 0 || !(partsort ? sift_win_ps_type_ok(in->col[slot].type) : is_key_type(&in->col[slot])))
			return false;
		slots[(*n)++] = slot;
	}
	return true;
}

/* the integer an int2, int4, date, int8, timestamp or timestamptz Datum holds, and the Datum of such an integer */
static int64
datum_int(Oid typid, Datum d)
{
	switch (typid)
	{
		case INT2OID:
			return DatumGetInt16(d);
		case INT4OID:
		case DATEOID:
			return DatumGetInt32(d);
		default:
			return DatumGetInt64(d);
	}
}

static Datum
int_datum(Oid typid, int64 v)
{
	switch (typid)
	{
		case INT2OID:
			return Int16GetDatum((int16) v);
		case INT4OID:
		case DATEOID:
			return Int32GetDatum((int32) v);
		default:
			return Int64GetDatum(v);
	}
}

static bool
lag_value(const Node *n, Oid typid, SiftWinFuncSpec *fs)
{
	const Const *c = (const Const *) n;

	if (!IsA(n, Const) || c->consttype != typid)
		return false;
	if (c->constisnull)
		return true;
	fs->has_default = true;
	if (typid == FLOAT8OID)
		fs->default_val.f = DatumGetFloat8(c->constvalue);
	else
		fs->default_val.i = datum_int(typid, c->constvalue);
	return true;
}

/* the name of a window function of pg_catalog and the kind of the engine that runs it */
static bool
kind_of(const WindowFunc *wf, SiftWinFuncKind *kind)
{
	static const struct
	{
		const char *name;
		SiftWinFuncKind kind;
	}			kinds[] = {{"row_number", SWIN_ROW_NUMBER}, {"rank", SWIN_RANK}, {"dense_rank", SWIN_DENSE_RANK}, {"lag", SWIN_LAG}, {"lead", SWIN_LEAD},
	{"first_value", SWIN_FIRST_VALUE}, {"count", SWIN_COUNT}, {"sum", SWIN_SUM}, {"avg", SWIN_AVG}, {"min", SWIN_MIN}, {"max", SWIN_MAX}};
	const char *name;

	if (get_func_namespace(wf->winfnoid) != PG_CATALOG_NAMESPACE || (name = get_func_name(wf->winfnoid)) == NULL)
		return false;
	for (int i = 0; i < lengthof(kinds); i++)
		if (strcmp(name, kinds[i].name) == 0)
			return (*kind = kinds[i].kind, true);
	return false;
}

/* the type PostgreSQL gives the result of the function over an argument of type arg */
static Oid
result_type(SiftWinFuncKind kind, Oid arg)
{
	switch (kind)
	{
		case SWIN_ROW_NUMBER:
		case SWIN_RANK:
		case SWIN_DENSE_RANK:
		case SWIN_COUNT_STAR:
		case SWIN_COUNT:
			return INT8OID;
		case SWIN_SUM:
			return arg == INT2OID || arg == INT4OID ? INT8OID : arg == INT8OID ? NUMERICOID : FLOAT8OID;
		case SWIN_AVG:
			return arg == FLOAT8OID ? FLOAT8OID : NUMERICOID;
		default:
			return arg;
	}
}

/* the argument types of a function: numbers for all of them, and for the functions that only compare or hand back a value a date or a timestamp too (an integer here) */
static bool
arg_type_ok(SiftWinFuncKind kind, Oid typid)
{
	switch (typid)
	{
		case INT2OID:
		case INT4OID:
		case INT8OID:
		case FLOAT8OID:
			return true;
		case DATEOID:
		case TIMESTAMPOID:
		case TIMESTAMPTZOID:
			return kind != SWIN_SUM && kind != SWIN_AVG;
		default:
			return kind == SWIN_COUNT;
	}
}

static bool
func_spec(SiftV6Input *in, const WindowFunc *wf, bool range, SiftV6WindowSpec *s, int f)
{
	SiftWinFuncSpec *fs = &s->win.funcs[f];
	const int	nargs = list_length(wf->args);
	SiftWinFuncKind kind;
	const bool	takes_arg = nargs > 0 && !wf->winstar;

	*fs = (SiftWinFuncSpec) {0};
	s->arg_slot[f] = -1;
	if (wf->aggfilter != NULL || !kind_of(wf, &kind))
		return false;
	if (kind == SWIN_COUNT && wf->winstar)
		kind = SWIN_COUNT_STAR;
	fs->kind = kind;
	if (takes_arg)
	{
		const Node *a = linitial(wf->args);
		int			slot;

		if (!IsA(a, Var) || (slot = sift_v6_input_add_var(in, (Var *) a)) < 0 || !is_arg_type(&in->col[slot]))
			return false;
		s->arg_slot[f] = slot;
		s->arg_typid[f] = in->col[slot].typid;
		fs->arg_type = in->col[slot].type;
		if (!arg_type_ok(kind, in->col[slot].typid))
			return false;
	}
	if (kind >= SWIN_COUNT_STAR)
		fs->range_frame = range;
	if (kind == SWIN_LAG || kind == SWIN_LEAD)
	{
		const Const *off = nargs > 1 ? lsecond(wf->args) : NULL;

		fs->offset = 1;
		if (nargs > 1 && (!IsA(off, Const) || off->constisnull || off->consttype != INT4OID || DatumGetInt32(off->constvalue) < 0 ||
						  DatumGetInt32(off->constvalue) > SIFT_WIN_MAX_OFFSET))
			return false;
		if (nargs > 1)
			fs->offset = (uint32) DatumGetInt32(off->constvalue);
		if (nargs > 2 && !lag_value(lthird(wf->args), s->arg_typid[f], fs))
			return false;
	}
	if ((takes_arg != (kind != SWIN_ROW_NUMBER && kind != SWIN_RANK && kind != SWIN_DENSE_RANK && kind != SWIN_COUNT_STAR)) ||
		(kind != SWIN_LAG && kind != SWIN_LEAD && nargs > 1) || nargs > 3)
		return false;
	s->res_typid[f] = result_type(kind, s->arg_typid[f]);
	return wf->wintype == s->res_typid[f];
}

/* The bound of the run condition of this execution: rn <= K, or rn < K + 1 */
static int64
bound_value(const List *bound, ParamListInfo params)
{
	const Node *n = linitial(bound);
	const bool	strict = DatumGetInt64(((const Const *) lsecond(bound))->constvalue) != 0;
	int64		k = 1;

	if (IsA(n, Const))
		k = ((const Const *) n)->consttype == INT4OID ? DatumGetInt32(((const Const *) n)->constvalue) : DatumGetInt64(((const Const *) n)->constvalue);
	else if (params != NULL)
	{
		const int	id = ((const Param *) n)->paramid;
		ParamExternData work;
		const ParamExternData *prm = params->paramFetch != NULL ? params->paramFetch(params, id, false, &work) : &params->params[id - 1];

		k = prm->isnull ? 0 : prm->ptype == INT4OID ? DatumGetInt32(prm->value) : DatumGetInt64(prm->value);
		k += strict;
	}
	return k - strict;
}

/* ORDER BY keys of a window that sorts its partitions: the flags of key j in the private ints */
static void
ps_order(const List *ints, SiftV6WindowSpec *out)
{
	if (!out->partsort)
		return;
	for (int j = 0; j < out->win.n_ord; j++)
		out->ps.ord[j] = (SiftWinPsKey) {.slot = out->ord_slot[j],.desc = list_nth_int(ints, 6 + j) & 1,.nulls_first = (list_nth_int(ints, 6 + j) >> 1) & 1};
}

bool
sift_v6_window_spec(const List *priv, SiftV6Input *in, ParamListInfo params, SiftV6WindowSpec *out)
{
	const List *ints = list_nth(priv, PRIV_INTS);
	const List *funcs = list_nth(priv, PRIV_FUNCS);
	const List *bound = list_nth(priv, PRIV_BOUND);
	const int	topk_func = list_nth_int(ints, 3);
	const int	mode = list_nth_int(ints, 5);
	const bool	ps = mode != WIN_EXACT;
	int			f = 0;

	*out = (SiftV6WindowSpec) {.indexoid = (Oid) list_nth_int(ints, 0),.rti = (Index) list_nth_int(ints, 1),.partsort = ps,.regroup = mode == WIN_REGROUP};
	out->win.mode = (SiftWinMode) list_nth_int(ints, 2);
	out->win.limit = SIFT_WIN_NO_LIMIT;
	if (sift_v6_winx_wanted(funcs))
	{
		out->winx = palloc0(sizeof(SiftV6Winx));
		out->win.n_funcs = (uint16) list_length(funcs);
		if (!(out->win.mode == SWIN_MODE_WINDOW && topk_func < 0 && add_keys(in, list_nth(priv, PRIV_PART), out->part_slot, &out->win.n_part, ps) &&
			  add_keys(in, list_nth(priv, PRIV_ORD), out->ord_slot, &out->win.n_ord, ps) && sift_v6_winx_spec(funcs, list_nth_int(ints, 4) != 0, in, out->winx)))
			return false;
		out->winx->spec.n_part = out->win.n_part;
		out->winx->spec.n_ord = out->win.n_ord;
		ps_order(ints, out);
		return true;
	}
	if (list_length(funcs) > SIFT_WIN_MAX_FUNCS ||
		!add_keys(in, list_nth(priv, PRIV_PART), out->part_slot, &out->win.n_part, ps) || !add_keys(in, list_nth(priv, PRIV_ORD), out->ord_slot, &out->win.n_ord, ps))
		return false;
	ps_order(ints, out);
	foreach_node(WindowFunc, wf, funcs)
	{
		if (!func_spec(in, wf, list_nth_int(ints, 4) != 0, out, f++))
			return false;
	}
	out->win.n_funcs = (uint16) f;
	if (topk_func >= 0)
	{
		const SiftWinFuncKind k = out->win.funcs[topk_func].kind;

		if (bound == NIL || (k != SWIN_ROW_NUMBER && k != SWIN_RANK && k != SWIN_DENSE_RANK))
			return false;
		out->win.topk = (SiftWinTopK) {.active = true,.func = (uint16) topk_func,.k = (uint64) Max(bound_value(bound, params), 1)};
		out->empty = bound_value(bound, params) < 1;
	}
	return true;
}

void
sift_v6_window_partsort(SiftV6WindowSpec *spec, const SiftV6Input *in)
{
	SiftWinPsSpec *ps = &spec->ps;
	const bool	row_number = spec->win.topk.active && spec->win.funcs[spec->win.topk.func].kind == SWIN_ROW_NUMBER;

	ps->ncols = in->ncols;
	for (int i = 0; i < in->ncols; i++)
	{
		ps->type[i] = in->col[i].type;
		ps->scale[i] = in->col[i].scale;
	}
	ps->n_part = spec->win.n_part;
	memcpy(ps->part_slot, spec->part_slot, sizeof(spec->part_slot));
	ps->n_ord = spec->win.n_ord;
	ps->max_bytes = (uint64) work_mem * 1024 / 4;
	ps->keep = row_number ? spec->win.topk.k : 0;
	spec->part_slot[0] = sift_win_ps_part_slot(ps);
	spec->win.n_part = 1;
	for (int j = 0; j < spec->win.n_ord; j++)
		spec->ord_slot[j] = sift_win_ps_peer_slot(ps, j);
	if (spec->winx != NULL)
	{
		spec->winx->spec.n_part = 1;
		spec->winx->spec.n_ord = spec->win.n_ord;
	}
}

Datum
sift_v6_window_datum(const SiftV6WindowSpec *s, int f, const SiftWinVal *v)
{
	const SiftWinFuncSpec *fs = &s->win.funcs[f];

	switch (fs->kind)
	{
		case SWIN_SUM:
			if (fs->arg_type == SVEC_F64)
				return Float8GetDatum(v->f);
			if (s->res_typid[f] == NUMERICOID)
				return sift_v6_numeric_from_i128(v->w, 0);
			if (v->w.hi != ((int64) v->w.lo < 0 ? -1 : 0))
				ereport(ERROR, (errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE), errmsg("bigint out of range")));
			return Int64GetDatum((int64) v->w.lo);
		case SWIN_AVG:
			return fs->arg_type == SVEC_F64 ? Float8GetDatum(v->f) : sift_v6_numeric_from_i128(sift_win_num_coef(&v->num), v->num.scale);
		case SWIN_MIN:
		case SWIN_MAX:
		case SWIN_LAG:
		case SWIN_LEAD:
		case SWIN_FIRST_VALUE:
			return s->arg_typid[f] == FLOAT8OID ? Float8GetDatum(v->f) : int_datum(s->arg_typid[f], v->i);
		default:
			return Int64GetDatum(v->i);
	}
}

typedef struct Feed
{
	SiftV6Source *src;
	const SiftV6WindowSpec *spec;
	SiftWinCarry *carry;
	SiftV6Batch b;
} Feed;

static bool
feed_next(void *p, SiftWinBatch *out)
{
	Feed	   *fd = p;
	const SiftV6WindowSpec *s = fd->spec;

	if (!fd->src->next(fd->src, &fd->b))
		return false;
	if (fd->carry != NULL)
		sift_win_carry_put(fd->carry, &fd->b);
	*out = (SiftWinBatch) {.n = fd->b.n,.tag = fd->b.tag,.mask = fd->b.mask};
	for (int i = 0; i < s->win.n_part; i++)
		out->part[i] = &fd->b.col[s->part_slot[i]];
	for (int i = 0; i < s->win.n_ord; i++)
		out->ord[i] = &fd->b.col[s->ord_slot[i]];
	for (int f = 0; f < s->win.n_funcs; f++)
		out->arg[f] = s->arg_slot[f] >= 0 ? &fd->b.col[s->arg_slot[f]] : NULL;
	return true;
}

static void
feed_skip(void *p, const int64 *part_key, uint8 part_nulls)
{
	Feed	   *fd = p;

	fd->src->skip(fd->src, part_key, part_nulls);
}

void
sift_v6_window_run(MemoryContext cxt, const SiftV6WindowSpec *spec, SiftV6Source *src, SiftWinCarry *carry, SiftWinEmit emit, void *emit_arg, SiftWinStats *stats)
{
	Feed		fd = {.src = src,.spec = spec,.carry = carry};
	SiftWindowExec *w = sift_window_create(cxt, &spec->win, (uint64) work_mem * 1024 / 4, feed_next, src->skip != NULL ? feed_skip : NULL, &fd, emit, emit_arg);

	sift_window_run(w);
	if (stats != NULL)
		*stats = *sift_window_stats(w);
	sift_window_free(w);
}

/* ---- the planner ---- */

typedef struct WinPlan
{
	PlannerInfo *root;
	RelOptInfo *rel;
	RelOptInfo *out;
	const IndexOptInfo *index;
	List	   *part;
	List	   *ord;
	List	   *funcs;
	List	   *scan_tlist;
	List	   *quals;
	List	   *bound;
	int			mode;
	int			topk_func;
	int			range;
	WinPartMode part_mode;
	List	   *ord_flags;		/* with partsort: of each order key, desc | nulls_first << 1 */
} WinPlan;

/* the Var of a sort or group clause, NULL for another expression */
static Var *
clause_var(PlannerInfo *root, const SortGroupClause *sgc, Index relid)
{
	Node	   *e = (Node *) get_sortgroupclause_expr((SortGroupClause *) sgc, root->parse->targetList);

	return IsA(e, Var) && ((Var *) e)->varno == relid && ((Var *) e)->varlevelsup == 0 ? (Var *) e : NULL;
}

/* the sort of the parts: ascending, NULLs last (reverse: descending, NULLs first) */
static bool
sort_is(const SortGroupClause *sgc, bool reverse)
{
	bool		desc;

	return OidIsValid(get_equality_op_for_ordering_op(sgc->sortop, &desc)) && desc == reverse && sgc->nulls_first == reverse;
}

static bool
vars_of(WinPlan *p, const List *clauses, bool reverse, List **out)
{
	foreach_node(SortGroupClause, sgc, (List *) clauses)
	{
		Var		   *v = clause_var(p->root, sgc, p->rel->relid);

		if (v == NULL || !sort_is(sgc, reverse))
			return false;
		*out = lappend(*out, v);
	}
	return true;
}

/* the keys the parts are cut and ordered by without a sort of the partition: of the types the sorted index holds as the order of the sort */
static bool
int_vars(const List *vars)
{
	SiftV6InCol c;

	foreach_ptr(Var, v, (List *) vars)
		if (!sift_v6_vec_type(v->vartype, v->vartypmod, &c) || !is_key_type(&c))
			return false;
	return true;
}

static bool
frame_ok(const WindowClause *wc, int *range)
{
	const int	o = wc->frameOptions;
	const int	excl = FRAMEOPTION_EXCLUDE_CURRENT_ROW | FRAMEOPTION_EXCLUDE_GROUP | FRAMEOPTION_EXCLUDE_TIES;

	*range = (o & FRAMEOPTION_RANGE) != 0;
	return (o & (FRAMEOPTION_RANGE | FRAMEOPTION_ROWS)) != 0 && (o & FRAMEOPTION_START_UNBOUNDED_PRECEDING) && (o & FRAMEOPTION_END_CURRENT_ROW) && !(o & excl) &&
		wc->startOffset == NULL && wc->endOffset == NULL;
}

/* A count in a run condition: an int4 or int8 constant, or an external parameter of those types (its value is that of the execution) */
static bool
count_node(const Node *n, int64 *plan_value)
{
	const Const *c = (const Const *) n;
	const Param *p = (const Param *) n;

	*plan_value = 1;
	if (IsA(n, Param))
		return p->paramkind == PARAM_EXTERN && (p->paramtype == INT4OID || p->paramtype == INT8OID);
	if (!IsA(n, Const) || c->constisnull || (c->consttype != INT4OID && c->consttype != INT8OID))
		return false;
	return (*plan_value = c->consttype == INT4OID ? DatumGetInt32(c->constvalue) : DatumGetInt64(c->constvalue), true);
}

/* WHERE rn <= K as PostgreSQL left it in the run condition of the function (the outer clause is gone): the engine stops at K, so it is the only condition the node can honour; false for any other */
static bool
run_condition(WinPlan *p)
{
	int			i = 0;

	p->topk_func = -1;
	foreach_node(WindowFunc, wf, p->funcs)
	{
		foreach_node(WindowFuncRunCondition, rc, wf->runCondition)
		{
			const char *name = get_opname(rc->opno);
			const bool	upper = name != NULL && strcmp(name, rc->wfunc_left ? "<=" : ">=") == 0;
			const bool	strict = name != NULL && strcmp(name, rc->wfunc_left ? "<" : ">") == 0;
			int64		k;

			if (p->topk_func >= 0 || !(upper || strict) || !count_node((Node *) rc->arg, &k) || (IsA(rc->arg, Const) && k - strict < 1))
				return false;
			p->topk_func = i;
			p->bound = list_make2(copyObject(rc->arg), makeConst(INT8OID, -1, InvalidOid, sizeof(int64), Int64GetDatum(strict), false, true));
		}
		i++;
	}
	return true;
}

static List *
private_of(const WinPlan *p)
{
	List	   *ints = lappend_int(list_make5_int((int) p->index->indexoid, (int) p->rel->relid, p->mode, p->topk_func, p->range), p->part_mode);

	ints = list_concat(ints, list_copy(p->ord_flags));
	return lappend(list_make5(ints, p->scan_tlist, p->part, p->ord, p->funcs), p->bound);
}

static const CustomPathMethods window_path_methods = {"SiftAggV6Window", sift_agg_v6_window_plan_path, NULL};

/* the engine runs the plan and the parts are sorted by the keys: the path is added */
/* the Vars of the scan tlist: the columns the node gives back */
static List *
vars_of_tlist(const List *tlist)
{
	List	   *vars = NIL;

	foreach_node(TargetEntry, te, (List *) tlist)
		if (IsA(te->expr, Var))
			vars = lappend(vars, te->expr);
	return vars;
}

static bool
window_finish(WinPlan *p, List *pathkeys, double rows)
{
	SiftV6Input in;
	SiftV6WindowSpec spec;
	SiftV6Vexpr probe;
	SiftV6Vexpr filters[SIFT_WINX_MAX_FUNCS];
	int			atts[2 * SIFT_WIN_MAX_KEYS];
	int			n = 0;
	List	   *priv;
	Cost		open,
				total;

	foreach_node(RestrictInfo, ri, p->rel->baserestrictinfo)
	{
		if (ri->pseudoconstant)
			return false;
		p->quals = lappend(p->quals, ri->clause);
	}
	priv = private_of(p);
	sift_v6_input_init_info(&in, p->index);
	if (!sift_v6_window_spec(priv, &in, NULL, &spec) || !sift_v6_vexpr_quals(CurrentMemoryContext, p->quals, p->rel->relid, NULL, &in, &probe) ||
		(spec.winx != NULL && !sift_v6_winx_filters(CurrentMemoryContext, p->funcs, p->rel->relid, NULL, &in, filters)))
		return false;
	for (int i = 0; i < spec.win.n_part; i++)
		atts[n++] = in.col[spec.part_slot[i]].attno;
	for (int i = 0; p->part_mode == WIN_EXACT && i < spec.win.n_ord; i++)
		atts[n++] = in.col[spec.ord_slot[i]].attno;
	if (p->part_mode == WIN_REGROUP ? !sift_win_regroup_fits(&in, vars_of_tlist(p->scan_tlist), p->rel->rows, estimate_num_groups(p->root, p->part, p->rel->rows, NULL, NULL), (uint64) work_mem * 1024) :
		!sift_v6_format()->sorted_by(p->index->indexoid, atts, n))
		return false;
	sift_v6_scan_price(p->root, p->rel, p->index, 0, &open, &total);
	total += cpu_tuple_cost * p->rel->rows + cpu_operator_cost * p->rel->rows * (spec.win.n_funcs + list_length(p->quals));
	if (p->part_mode != WIN_EXACT)
		total += cpu_operator_cost * p->rel->rows * 2.0 * (1 + spec.win.n_ord);
	if (p->part_mode == WIN_REGROUP)
		total += cpu_operator_cost * p->rel->rows * 4.0;
	sift_v6_add_path(p->out, &window_path_methods, open, total, rows, pathkeys, lappend(list_copy(priv), p->quals));
	return true;
}

static bool
common_shape(PlannerInfo *root)
{
	const Query *q = root->parse;

	return sift_enable_agg && q->commandType == CMD_SELECT && !q->hasAggs && q->groupClause == NIL && q->groupingSets == NIL && !q->hasTargetSRFs &&
		q->setOperations == NULL && q->rowMarks == NIL;
}

/* the scan tlist of a window: the Vars the node outputs (plain columns of the table) and the functions of the one window; false for anything else */
static bool
collect_funcs(WinPlan *p, const WindowClause *wc)
{
	p->scan_tlist = sift_v6_scan_tlist(p->out->reltarget->exprs, p->rel->relid, T_WindowFunc);
	foreach_node(TargetEntry, te, p->scan_tlist)
	{
		if (IsA(te->expr, WindowFunc))
		{
			if (((WindowFunc *) te->expr)->winref != wc->winref)
				return false;
			p->funcs = lappend(p->funcs, te->expr);
		}
		else if (((Var *) te->expr)->varattno <= 0)
			return false;
	}
	return p->funcs != NIL;
}

/* The Vars of a PARTITION BY or an ORDER BY clause of any direction, and for an order the flags of each key; a text key only where the bytes are the order (the C collation) */
static bool
vars_any(WinPlan *p, const List *clauses, List **vars, List **flags)
{
	foreach_node(SortGroupClause, sgc, (List *) clauses)
	{
		Var		   *v = clause_var(p->root, sgc, p->rel->relid);
		bool		desc;

		if (v == NULL || !OidIsValid(get_equality_op_for_ordering_op(sgc->sortop, &desc)) || (sift_v6_class(v->vartype) == SV6C_TEXT && v->varcollid != C_COLLATION_OID))
			return false;
		*vars = lappend(*vars, v);
		if (flags != NULL)
			*flags = lappend_int(*flags, desc | (sgc->nulls_first << 1));
	}
	return true;
}

/*
 * The window over the index with the partitions made as mode says: from the parts that are sorted by the keys and the order (integer keys, no sort), by the keys (each partition is sorted), or by
 * neither (the rows of a partition are put together first, and the rows leave in no order: a path with no pathkeys, so that PostgreSQL sorts them when the statement wants them sorted).
 */
static bool
window_try(PlannerInfo *root, RelOptInfo *input_rel, RelOptInfo *output_rel, IndexOptInfo *ix, const WindowClause *wc, WinPartMode mode)
{
	WinPlan		p = {.root = root,.rel = input_rel,.out = output_rel,.index = ix,.mode = SWIN_MODE_WINDOW,.part_mode = mode};

	if (!frame_ok(wc, &p.range) || (mode == WIN_EXACT ? !(vars_of(&p, wc->partitionClause, false, &p.part) && vars_of(&p, wc->orderClause, false, &p.ord) && int_vars(p.part) && int_vars(p.ord)) :
		(wc->partitionClause == NIL || !vars_any(&p, wc->partitionClause, &p.part, NULL) || !vars_any(&p, wc->orderClause, &p.ord, &p.ord_flags) || (mode == WIN_REGROUP && !int_vars(p.part)))))
		return false;
	return collect_funcs(&p, wc) && run_condition(&p) && window_finish(&p, mode == WIN_REGROUP ? NIL : root->window_pathkeys, input_rel->rows);
}

static void
window_path(PlannerInfo *root, RelOptInfo *input_rel, RelOptInfo *output_rel)
{
	const Query *q = root->parse;
	const WindowClause *wc;

	if (!q->hasWindowFuncs || list_length(q->windowClause) != 1 || q->distinctClause != NIL)
		return;
	wc = linitial(q->windowClause);
	foreach_ptr(IndexOptInfo, ix, input_rel->indexlist)
	{
		if (!agg_index_complete(ix) || !sift_v6_format()->owns_rel(ix->indexoid, false) || wc->orderClause == NIL)
			continue;
		for (WinPartMode mode = WIN_EXACT; mode <= WIN_REGROUP; mode++)
			if (window_try(root, input_rel, output_rel, ix, wc, mode))
				return;
	}
}

static void
distinct_path(PlannerInfo *root, RelOptInfo *input_rel, RelOptInfo *output_rel)
{
	const Query *q = root->parse;
	const int	nd = list_length(q->distinctClause);

	if (!q->hasDistinctOn || q->hasWindowFuncs || q->sortClause == NIL || list_length(q->sortClause) < nd)
		return;
	foreach_ptr(IndexOptInfo, ix, input_rel->indexlist)
	{
		WinPlan		p = {.root = root,.rel = input_rel,.out = output_rel,.index = ix,.mode = SWIN_MODE_DISTINCT_FIRST,.topk_func = -1};
		List	   *rest = list_copy_tail(q->sortClause, nd);
		List	   *keys = list_copy_head(q->sortClause, nd);

		if (!agg_index_complete(ix) || !sift_v6_format()->owns_rel(ix->indexoid, false) || !vars_of(&p, keys, false, &p.part))
			continue;
		if (rest != NIL && !vars_of(&p, rest, false, &p.ord))
		{
			p.ord = NIL;
			p.mode = SWIN_MODE_DISTINCT_LAST;
			if (!vars_of(&p, rest, true, &p.ord))
				continue;
		}
		p.scan_tlist = sift_v6_scan_tlist(output_rel->reltarget->exprs, input_rel->relid, T_Invalid);
		foreach_node(TargetEntry, te, p.scan_tlist)
			if (((Var *) te->expr)->varattno <= 0)
				p.scan_tlist = NIL;
		if (p.scan_tlist != NIL && window_finish(&p, root->distinct_pathkeys, estimate_num_groups(root, (List *) p.part, input_rel->rows, NULL, NULL)))
			return;
	}
}

void
sift_agg_v6_window_upper_paths(PlannerInfo *root, UpperRelationKind stage, RelOptInfo *input_rel, RelOptInfo *output_rel, void *extra)
{
	if ((stage != UPPERREL_WINDOW && stage != UPPERREL_DISTINCT) || !common_shape(root) || !sift_v6_base_rel_ok(root, input_rel))
		return;
	sift_v6_stage_target(root, stage, output_rel);
	if (stage == UPPERREL_WINDOW)
		window_path(root, input_rel, output_rel);
	else
		distinct_path(root, input_rel, output_rel);
}

Plan *
sift_agg_v6_window_plan_path(PlannerInfo *root, RelOptInfo *rel, CustomPath *best, List *tlist, List *clauses, List *custom_plans)
{
	return sift_v6_plan_scan(root, best, tlist, &sift_agg_v6_window_plan_methods, (Oid) list_nth_int(linitial(best->custom_private), 0));
}

/* ---- the node ---- */

typedef struct WindowState
{
	CustomScanState css;
	List	   *priv;
	List	   *quals;
	SiftV6WindowSpec *spec;		/* of its own allocation: a 128-bit member needs more alignment than a node has */
	SiftV6Source *src;
	SiftV6Store out;
	SiftV6Vexpr *filters;		/* SiftWinx: the program of each FILTER clause */
	AttrNumber *attnos;			/* the columns fetched for a row: the Vars of the scan tlist */
	const Var **attvar;
	Datum	   *fetched;
	bool	   *fnull;
	SiftWinCarry *carry;		/* the columns of the rows the batches held, so that no row is fetched */
	int			nattrs;
	int		   *entry;			/* of each entry of the scan tlist: its place among attnos, or -1 - f for the result of function f */
	Datum	   *vals;
	bool	   *nulls;
	SiftWinStats stats;
} WindowState;

static void
window_begin(CustomScanState *node, EState *estate, int eflags)
{
	WindowState *st = (WindowState *) node;
	const CustomScan *cs = (CustomScan *) node->ss.ps.plan;
	List	   *tlist = lsecond(cs->custom_private);
	int			i = 0;

	st->priv = cs->custom_private;
	st->spec = palloc_aligned(sizeof(SiftV6WindowSpec), _Alignof(SiftV6WindowSpec), MCXT_ALLOC_ZERO);
	st->quals = list_length(cs->custom_private) > PRIV_BOUND + 1 ? list_nth(cs->custom_private, PRIV_BOUND + 1) : NIL;
	st->attnos = palloc(list_length(tlist) * sizeof(AttrNumber));
	st->attvar = palloc(list_length(tlist) * sizeof(Var *));
	st->fetched = palloc(list_length(tlist) * sizeof(Datum));
	st->fnull = palloc(list_length(tlist) * sizeof(bool));
	st->entry = palloc(list_length(tlist) * sizeof(int));
	st->vals = palloc(list_length(tlist) * sizeof(Datum));
	st->nulls = palloc(list_length(tlist) * sizeof(bool));
	foreach_node(TargetEntry, te, tlist)
	{
		int			f = 0;

		st->entry[i] = -1;
		if (IsA(te->expr, Var))
		{
			st->attvar[st->nattrs] = (Var *) te->expr;
			st->attnos[st->nattrs] = ((Var *) te->expr)->varattno;
			st->entry[i] = st->nattrs++;
		}
		else
			foreach_ptr(Node, wf, list_nth(cs->custom_private, PRIV_FUNCS))
			{
				if (equal(wf, te->expr))
				{
					st->entry[i] = -1 - f;
					break;
				}
				f++;
			}
		i++;
	}
}

/* The row of the window (tag: what the source fetches its columns by) with the result of each function as a Datum */
static void
window_put(WindowState *st, uint64 tag, const Datum *res, const bool *res_null)
{
	const List *tlist = lsecond(((CustomScan *) st->css.ss.ps.plan)->custom_private);
	MemoryContext old = MemoryContextSwitchTo(st->out.row_cx);
	Datum	   *fetched = st->fetched;
	bool	   *fnull = st->fnull;
	const bool	have = st->nattrs == 0 || (st->carry != NULL && sift_win_carry_take(st->carry, tag, fetched, fnull)) ||
		st->src->fetch(st->src, tag, st->attnos, st->nattrs, fetched, fnull);

	SIFT_INVARIANT(have, "a row of the window scan of index %u has no columns to fetch", st->spec->indexoid);
	for (int i = 0; i < list_length(tlist); i++)
	{
		const int	e = st->entry[i];

		if (e >= 0)
		{
			st->vals[i] = fetched[e];
			st->nulls[i] = fnull[e];
		}
		else
		{
			const int	f = -1 - e;

			st->nulls[i] = res_null[f];
			st->vals[i] = res[f];
		}
	}
	MemoryContextSwitchTo(old);
	sift_v6_store_put(&st->out, st->css.ss.ss_ScanTupleSlot->tts_tupleDescriptor, st->vals, st->nulls);
}

static void
window_emit(void *arg, const SiftWinRow *row)
{
	WindowState *st = arg;
	Datum		res[SIFT_WIN_MAX_FUNCS];
	bool		res_null[SIFT_WIN_MAX_FUNCS];

	for (int f = 0; f < st->spec->win.n_funcs; f++)
	{
		res_null[f] = (row->res_nulls >> f) & 1;
		res[f] = res_null[f] ? (Datum) 0 : sift_v6_window_datum(st->spec, f, &row->res[f]);
	}
	window_put(st, row->tag, res, res_null);
}

static void
winx_emit(void *arg, uint64 tag, const SiftWinxVal *v)
{
	WindowState *st = arg;
	Datum		res[SIFT_WINX_MAX_FUNCS];
	bool		res_null[SIFT_WINX_MAX_FUNCS];

	for (int f = 0; f < st->spec->win.n_funcs; f++)
	{
		res_null[f] = v[f].null;
		res[f] = v[f].null ? (Datum) 0 : sift_v6_winx_datum(st->spec->winx, f, &v[f]);
	}
	window_put(st, tag, res, res_null);
}

static void
window_release(WindowState *st)
{
	if (st->src != NULL)
		st->src->close(st->src);
	st->src = NULL;
	if (st->carry != NULL)
		sift_win_carry_free(st->carry);
	st->carry = NULL;
	sift_v6_store_end(&st->out);
}

static void
window_run(WindowState *st, EState *estate)
{
	SiftV6Input in;
	SiftV6Vexpr quals;
	bool		compiled;
	Relation	index = index_open((Oid) list_nth_int(linitial(st->priv), 0), AccessShareLock);

	sift_v6_store_start(&st->out, estate);
	sift_v6_input_init_rel(&in, index);
	sift_v6_input_bind(&in, estate);
	index_close(index, NoLock);
	st->filters = palloc0(SIFT_WINX_MAX_FUNCS * sizeof(SiftV6Vexpr));
	compiled = sift_v6_window_spec(st->priv, &in, estate->es_param_list_info, st->spec) && sift_v6_vexpr_quals(st->out.cx, st->quals, st->spec->rti, estate->es_param_list_info, &in, &quals) &&
		(st->spec->winx == NULL || sift_v6_winx_filters(st->out.cx, list_nth(st->priv, PRIV_FUNCS), st->spec->rti, estate->es_param_list_info, &in, st->filters));
	SIFT_INVARIANT(compiled, "the window scan of index %u is not compiled", st->spec->indexoid);
	if (st->spec->empty)
	{
		st->out.ran = true;
		return;
	}
	st->carry = st->spec->winx != NULL ? NULL : sift_win_carry_open(st->out.cx, &in, st->attvar, st->nattrs, (uint64) work_mem * 1024 / 4);
	st->src = sift_v6_format()->open(estate, &in, st->quals == NIL ? NULL : &quals, !st->spec->regroup);
	if (st->spec->regroup)
		st->src = sift_win_regroup_over(st->out.cx, st->src, &in, st->spec->win.n_part, st->spec->part_slot, (uint64) work_mem * 1024);
	if (st->spec->partsort)
	{
		sift_v6_window_partsort(st->spec, &in);
		st->src = sift_win_partsort_open(st->out.cx, st->src, &st->spec->ps);
	}
	if (st->spec->winx != NULL)
	{
		SiftWinxStats wx;

		sift_v6_winx_run(st->out.cx, st->spec->winx, st->spec->part_slot, st->spec->ord_slot, st->src, st->filters, winx_emit, st, &wx);
		st->stats = (SiftWinStats) {.rows_in = wx.rows,.rows_out = wx.rows,.max_pending = wx.max_pending};
	}
	else
		sift_v6_window_run(st->out.cx, st->spec, st->src, st->carry, window_emit, st, &st->stats);
	st->out.ran = true;
}

static TupleTableSlot *
window_next(ScanState *ss)
{
	WindowState *st = (WindowState *) ss;

	if (!st->out.ran)
		window_run(st, ss->ps.state);
	return sift_v6_store_get(&st->out, ss->ss_ScanTupleSlot);
}

static bool
window_recheck(ScanState *ss, TupleTableSlot *slot)
{
	return true;
}

static TupleTableSlot *
window_exec(CustomScanState *node)
{
	return ExecScan(&node->ss, window_next, window_recheck);
}

static void
window_end(CustomScanState *node)
{
	window_release((WindowState *) node);
}

/* the same parameters give the same rows: a scan again reads the store from its start; other parameters make the engine run again */
static void
window_rescan(CustomScanState *node)
{
	WindowState *st = (WindowState *) node;

	if (node->ss.ps.chgParam != NULL && st->src != NULL)
		window_release(st);
	sift_v6_store_rescan(&st->out, node->ss.ps.chgParam != NULL);
}

static void
window_explain(CustomScanState *node, List *ancestors, ExplainState *es)
{
	WindowState *st = (WindowState *) node;

	sift_v6_explain_index(es, (Oid) list_nth_int(linitial(st->priv), 0));
	if (st->out.ran)
	{
		sift_v6_explain_count(es, "Rows In", (int64) st->stats.rows_in);
		sift_v6_explain_count(es, "Rows Out", (int64) st->stats.rows_out);
		sift_v6_explain_count(es, "Partitions Skipped", (int64) st->stats.skips);
	}
}

static const CustomExecMethods window_exec_methods = {
	.CustomName = "SiftAggV6Window",
	.BeginCustomScan = window_begin,
	.ExecCustomScan = window_exec,
	.EndCustomScan = window_end,
	.ReScanCustomScan = window_rescan,
	.ExplainCustomScan = window_explain,
};

static Node *
window_create_state(CustomScan *cscan)
{
	WindowState *st = (WindowState *) newNode(sizeof(WindowState), T_CustomScanState);

	st->css.methods = &window_exec_methods;
	st->css.slotOps = &TTSOpsMinimalTuple;
	return (Node *) st;
}

const CustomScanMethods sift_agg_v6_window_plan_methods = {"SiftAggV6Window", window_create_state};
