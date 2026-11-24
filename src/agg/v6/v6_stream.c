/*
 * agg/v6/v6_stream.c - the path and the node of SiftAggV6Stream. As in v6_window.c the plan's private list describes the node and sift_v6_stream_spec makes the spec of the engine
 * (agg/stream_exec.h) from it, in the planner to know that the engine runs the query and in the node to run it.
 */
#include "postgres.h"

#include "access/genam.h"
#include "catalog/pg_aggregate.h"
#include "catalog/pg_namespace_d.h"
#include "catalog/pg_type_d.h"
#include "utils/builtins.h"
#include "utils/datetime.h"
#include "utils/fmgrprotos.h"
#include "utils/timestamp.h"
#include "executor/executor.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "optimizer/cost.h"
#include "optimizer/optimizer.h"
#include "parser/parsetree.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/rel.h"
#include "utils/selfuncs.h"

#include "agg/agg.h"
#include "agg/group/state.h"
#include "agg/v6/v6_safety.h"
#include "agg/v6/v6_source_rel.h"
#include "agg/v6/v6_card.h"
#include "agg/v6/v6_datebin_win_pg.h"
#include "agg/v6/v6_plan_shape.h"
#include "agg/v6/v6_fifo.h"
#include "agg/v6/v6_stream.h"
#include "agg/v6/v6_settings.h"

enum
{
	PRIV_INTS = 0,				/* the oid of the source, rti, the source is a sift_table */
	PRIV_TLIST = 1,				/* the scan tlist: sift_v6_plan_scan reads it from here */
	PRIV_KEYS = 2,
	PRIV_AGGS = 3,
	PRIV_HAVING = 4,
	PRIV_QUALS = 5
};

static bool
agg_kind(const Aggref *a, SiftV6Kind *kind)
{
	const char *name = get_func_name(a->aggfnoid);

	if (name == NULL || get_func_namespace(a->aggfnoid) != PG_CATALOG_NAMESPACE || a->aggorder != NIL || a->aggdistinct != NIL ||
		a->aggkind != AGGKIND_NORMAL || a->aggvariadic || a->agglevelsup != 0)
		return false;
	if (strcmp(name, "count") == 0)
		*kind = a->aggstar ? SV6_COUNT_STAR : SV6_COUNT;
	else if (strcmp(name, "sum") == 0)
		*kind = SV6_SUM;
	else if (strcmp(name, "avg") == 0)
		*kind = SV6_AVG;
	else if (strcmp(name, "min") == 0)
		*kind = SV6_MIN;
	else if (strcmp(name, "max") == 0)
		*kind = SV6_MAX;
	else
		return false;
	return true;
}

static bool
key_type(const SiftV6InCol *c)
{
	return c->type == SVEC_I32 || c->type == SVEC_I64;
}

static bool
is_numeric(const SiftV6InCol *c)
{
	return c->type == SVEC_NUM64;
}

static bool
value_type(const SiftV6InCol *c)
{
	return key_type(c) || is_numeric(c) || (c->type == SVEC_F64 && c->typid == FLOAT8OID);
}

/* The type PostgreSQL gives the aggregate over a column of this one; InvalidOid: the engine does not take it */
static Oid
result_type(SiftV6Kind kind, const SiftV6InCol *c)
{
	const bool	summable = c->typid == INT2OID || c->typid == INT4OID || c->typid == INT8OID || is_numeric(c) || c->typid == FLOAT8OID;

	switch (kind)
	{
		case SV6_COUNT:
			return INT8OID;
		case SV6_SUM:
			return !summable ? InvalidOid : c->typid == FLOAT8OID ? FLOAT8OID : (is_numeric(c) || c->typid == INT8OID) ? NUMERICOID : INT8OID;
		case SV6_AVG:
			return !summable ? InvalidOid : c->typid == FLOAT8OID ? FLOAT8OID : NUMERICOID;
		default:
			return c->typid;
	}
}

static int
filter_slot(SiftV6StreamSpec *s, const Node *f)
{
	if (f == NULL)
		return -1;
	for (int i = 0; i < s->nfilters; i++)
		if (equal(s->filter[i], f))
			return i;
	if (s->nfilters == SIFT_V6_STREAM_MAX_FILTERS)
		return -2;
	s->filter[s->nfilters] = f;
	return s->nfilters++;
}

static int
val_slot(SiftV6StreamSpec *s, int slot, int filter, const SiftV6InCol *c)
{
	for (int i = 0; i < s->nvals; i++)
		if (s->val[i].slot == slot && s->val[i].filter == filter)
			return i;
	if (s->nvals == SIFT_STREAM_MAX_VALS)
		return -1;
	s->val[s->nvals] = (SiftV6StreamVal) {.slot = slot,.filter = filter,.typid = c != NULL ? c->typid : InvalidOid,.scale = c != NULL ? c->scale : 0,
	.is_f64 = c != NULL && c->type == SVEC_F64};
	return s->nvals++;
}

static bool
add_agg(SiftV6Input *in, const Aggref *a, SiftV6StreamSpec *s)
{
	SiftV6StreamAgg *g = &s->agg[s->naggs];
	const Node *arg;
	const SiftV6InCol *c = NULL;
	int			slot = -1,
				filter = filter_slot(s, (const Node *) a->aggfilter);

	if (s->naggs == SIFT_V6_STREAM_MAX_AGGS || filter == -2 || !agg_kind(a, &g->kind))
		return false;
	g->restype = a->aggtype;
	g->val = -1;
	if (g->kind == SV6_COUNT_STAR)
	{
		if (list_length(a->args) != 0 || g->restype != INT8OID)
			return false;
		g->val = filter < 0 ? -1 : val_slot(s, -1, filter, NULL);
		s->naggs += g->val >= -1;
		return g->val >= -1 && (filter < 0 || g->val >= 0);
	}
	arg = list_length(a->args) == 1 ? (Node *) ((TargetEntry *) linitial(a->args))->expr : NULL;
	if (arg == NULL || !IsA(arg, Var) || (slot = sift_v6_input_add_var(in, (Var *) arg)) < 0 || !value_type(&in->col[slot]))
		return false;
	c = &in->col[slot];
	if (result_type(g->kind, c) != g->restype)
		return false;
	g->val = val_slot(s, slot, filter, c);
	s->naggs += g->val >= 0;
	return g->val >= 0;
}

static bool
bin_of(SiftV6Input *in, const Expr *e, SiftV6StreamSpec *s, int *slot)
{
	const FuncExpr *f = (const FuncExpr *) e;
	const char *name;
	SiftDateBin bin;
	const Var  *v;

	if (!IsA(e, FuncExpr) || get_func_namespace(f->funcid) != PG_CATALOG_NAMESPACE || (name = get_func_name(f->funcid)) == NULL)
		return false;
	if (strcmp(name, "date_bin") == 0 && list_length(f->args) == 3 && IsA(lsecond(f->args), Var))
	{
		const Const *stride = linitial(f->args),
				   *origin = lthird(f->args);
		SiftDateBinShape shape = {.stride_const = IsA(stride, Const),.origin_const = IsA(origin, Const)};

		v = lsecond(f->args);
		if (!shape.stride_const || !shape.origin_const || (*slot = sift_v6_input_add_var(in, v)) < 0)
			return false;
		shape.stride_null = stride->constisnull;
		shape.origin_null = origin->constisnull;
		if (!shape.stride_null)
		{
			const Interval *iv = DatumGetIntervalP(stride->constvalue);

			shape.stride = (SiftInterval16) {.time = iv->time,.day = iv->day,.month = iv->month};
		}
		shape.origin = origin->constisnull ? 0 : DatumGetInt64(origin->constvalue);
		shape.source_ok = (v->vartype == TIMESTAMPOID || v->vartype == TIMESTAMPTZOID) && origin->consttype == v->vartype;
		if (sift_shape_datebin(&shape, &bin) != SSV_OK)
			return false;
		SIFT_INVARIANT(s->nbins < SIFT_STREAM_MAX_KEYS, "a stream has more bins than keys");
		s->bin[s->nbins++] = (SiftV6StreamBin) {.key = s->nkeys,.stride_us = bin.stride_us,.origin = bin.origin};
		return true;
	}
	if (strcmp(name, "date_trunc") == 0 && list_length(f->args) == 2 && IsA(linitial(f->args), Const) && IsA(lsecond(f->args), Var))
	{
		const Const *unit = linitial(f->args);
		const char *u;
		int64		stride;

		v = lsecond(f->args);
		if (unit->constisnull || v->vartype != TIMESTAMPOID || (*slot = sift_v6_input_add_var(in, v)) < 0)
			return false;
		u = TextDatumGetCString(unit->constvalue);
		stride = strcmp(u, "second") == 0 ? USECS_PER_SEC : strcmp(u, "minute") == 0 ? USECS_PER_MINUTE : strcmp(u, "hour") == 0 ? USECS_PER_HOUR :
			strcmp(u, "day") == 0 ? USECS_PER_DAY : strcmp(u, "week") == 0 ? 7 * USECS_PER_DAY : 0;
		if (stride == 0)
			return false;
		SIFT_INVARIANT(s->nbins < SIFT_STREAM_MAX_KEYS, "a stream has more bins than keys");
		s->bin[s->nbins++] = (SiftV6StreamBin) {.key = s->nkeys,.stride_us = stride,.origin = strcmp(u, "week") == 0 ? 366 * USECS_PER_DAY : 0};
		return true;
	}
	return false;
}

bool
sift_v6_stream_spec(const List *priv, SiftV6Input *in, SiftV6StreamSpec *out)
{
	const List *ints = list_nth(priv, PRIV_INTS);
	const List *keys = list_nth(priv, PRIV_KEYS);

	*out = (SiftV6StreamSpec) {.indexoid = (Oid) list_nth_int(ints, 0),.rti = (Index) list_nth_int(ints, 1)};
	if (list_length(keys) < 1 || list_length(keys) > SIFT_STREAM_MAX_KEYS)
		return false;
	foreach_ptr(Expr, k, (List *) keys)
	{
		int			slot;

		if (IsA(k, Var))
			slot = sift_v6_input_add_var(in, (Var *) k);
		else if (!bin_of(in, k, out, &slot))
			return false;
		if (slot < 0 || !key_type(&in->col[slot]))
			return false;
		out->key_slot[out->nkeys] = slot;
		out->key_typid[out->nkeys++] = exprType((Node *) k);
	}
	foreach_node(Aggref, a, list_nth(priv, PRIV_AGGS))
		if (!add_agg(in, a, out))
			return false;
	return out->naggs > 0;
}

bool
sift_v6_stream_filters(MemoryContext cxt, const SiftV6StreamSpec *spec, ParamListInfo params, SiftV6Input *in, SiftV6Vexpr *out)
{
	for (int i = 0; i < spec->nfilters; i++)
		if (!sift_v6_vexpr_compile(cxt, spec->filter[i], spec->rti, params, in, &out[i]))
			return false;
	return true;
}

Datum
sift_v6_stream_key_datum(const SiftV6StreamSpec *s, int i, const SiftStreamGroup *g)
{
	switch (s->key_typid[i])
	{
		case INT2OID:
			return Int16GetDatum((int16) g->key[i]);
		case INT4OID:
		case DATEOID:
			return Int32GetDatum((int32) g->key[i]);
		default:
			return Int64GetDatum(g->key[i]);
	}
}

static Datum
val_datum(Oid typid, int64 v)
{
	return typid == INT2OID ? Int16GetDatum((int16) v) : typid == INT4OID || typid == DATEOID ? Int32GetDatum((int32) v) : Int64GetDatum(v);
}

static Datum
average(const SiftV6StreamVal *c, const SiftStreamVal *v)
{
	SiftNumAvg	avg;

	if (sift_numavg_exact(v->sum, v->n, c->scale, &avg) && avg.ok)
		return sift_numavg_datum(&avg);
	return DirectFunctionCall2(numeric_div, sift_v6_numeric_from_i128(v->sum, c->scale), DirectFunctionCall1(int8_numeric, Int64GetDatum((int64) v->n)));
}

static Datum
sum_datum(const SiftV6StreamAgg *a, const SiftV6StreamVal *c, const SiftStreamVal *v)
{
	if (c->is_f64)
		return Float8GetDatum(v->fsum);
	if (a->restype == NUMERICOID)
		return sift_v6_numeric_from_i128(v->sum, c->scale);
	if (v->sum.hi != ((int64) v->sum.lo < 0 ? -1 : 0))
		ereport(ERROR, (errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE), errmsg("bigint out of range")));
	return Int64GetDatum((int64) v->sum.lo);
}

bool
sift_v6_stream_datum(const SiftV6StreamSpec *s, int j, const SiftStreamGroup *g, Datum *out)
{
	const SiftV6StreamAgg *a = &s->agg[j];
	const SiftV6StreamVal *c = a->val >= 0 ? &s->val[a->val] : NULL;
	const SiftStreamVal *v = c != NULL ? &g->v[a->val] : NULL;

	if (a->kind == SV6_COUNT_STAR || a->kind == SV6_COUNT)
	{
		*out = Int64GetDatum((int64) (v != NULL ? v->n : g->rows));
		return true;
	}
	if (v->n == 0)
		return false;
	if (a->kind == SV6_SUM)
		*out = sum_datum(a, c, v);
	else if (a->kind == SV6_AVG)
		*out = c->is_f64 ? Float8GetDatum(v->fsum / (double) v->n) : average(c, v);
	else if (c->is_f64)
		*out = Float8GetDatum(a->kind == SV6_MIN ? v->fmin : v->fmax);
	else if (a->restype == NUMERICOID)
		*out = sift_v6_numeric_from_i128(sift_i128_of_i64(a->kind == SV6_MIN ? v->min : v->max), c->scale);
	else
		*out = val_datum(c->typid, a->kind == SV6_MIN ? v->min : v->max);
	return true;
}

typedef struct Feed
{
	SiftV6Source *src;
	const SiftV6StreamSpec *spec;
	const SiftV6Vexpr *filters;
	SiftVexprCtx *vctx;
	SiftV6Batch b;
	SiftBatchSel bs;
	SiftDateBin bin[SIFT_STREAM_MAX_KEYS];
	SiftVec		binvec[SIFT_STREAM_MAX_KEYS];
	int64	   *binout[SIFT_STREAM_MAX_KEYS];
	const SiftVec *keyvec[SIFT_STREAM_MAX_KEYS];
	uint64		fbits[SIFT_V6_STREAM_MAX_FILTERS][SIFT_VEC_WORDS];
} Feed;

static void
batch_sel(Feed *fd)
{
	const int	n = fd->b.n,
				words = (n + 63) / 64;
	uint16		active = 0;

	memset(fd->bs.mask, 0, sizeof(fd->bs.mask));
	if (fd->b.mask != NULL)
		memcpy(fd->bs.mask, fd->b.mask, words * sizeof(uint64));
	else
		for (int i = 0; i < n; i++)
			fd->bs.mask[i >> 6] |= UINT64CONST(1) << (i & 63);
	for (int w = 0; w < words; w++)
		active += pg_popcount64(fd->bs.mask[w]);
	fd->bs.n_raw = n;
	fd->bs.n_active = active;
	fd->bs.all_active = active == n;
	fd->bs.has_sel = false;
}

static void
bin_keys(Feed *fd)
{
	batch_sel(fd);
	for (int k = 0; k < fd->spec->nbins; k++)
	{
		const int	key = fd->spec->bin[k].key;
		const SiftVec *src = &fd->b.col[fd->spec->key_slot[key]];
		uint32		err_row;
		const SiftDateBinStatus st = sift_datebin_batch(&fd->bin[k], &fd->bs, src, fd->binout[k], &err_row);

		if (st != SDB_OK)
			sift_datebin_raise(st);
		fd->binvec[k] = *src;
		fd->binvec[k].data = fd->binout[k];
		fd->keyvec[key] = &fd->binvec[k];
	}
}

static bool
feed_next(void *p, SiftStreamBatch *out)
{
	Feed	   *fd = p;
	const SiftV6StreamSpec *s = fd->spec;

	if (!fd->src->next(fd->src, &fd->b))
		return false;
	for (int i = 0; i < s->nkeys; i++)
		fd->keyvec[i] = &fd->b.col[s->key_slot[i]];
	bin_keys(fd);
	for (int f = 0; f < s->nfilters; f++)
		sift_v6_vexpr_filter(fd->vctx, &fd->filters[f], &fd->b, fd->fbits[f]);
	*out = (SiftStreamBatch) {.n = fd->b.n,.mask = fd->b.mask,.nvals = (uint8) s->nvals};
	for (int i = 0; i < s->nkeys; i++)
		out->keys[i] = fd->keyvec[i];
	for (int j = 0; j < s->nvals; j++)
	{
		out->val[j] = s->val[j].slot >= 0 ? &fd->b.col[s->val[j].slot] : NULL;
		out->filter[j] = s->val[j].filter >= 0 ? fd->fbits[s->val[j].filter] : NULL;
	}
	return true;
}

struct SiftV6StreamRun
{
	Feed	   *fd;
	SiftStreamExec se;
};

SiftV6StreamRun *
sift_v6_stream_open(MemoryContext cx, SiftV6Source *src, const SiftV6StreamSpec *spec, const SiftV6Vexpr *filters, void (*emit) (void *, const SiftStreamGroup *), void *emit_arg)
{
	SiftV6StreamRun *r = MemoryContextAllocAligned(cx, sizeof(SiftV6StreamRun), _Alignof(SiftV6StreamRun), MCXT_ALLOC_ZERO);
	Feed	   *fd = MemoryContextAllocAligned(cx, sizeof(Feed), _Alignof(Feed), MCXT_ALLOC_ZERO);

	fd->src = src;
	fd->spec = spec;
	fd->filters = filters;
	fd->vctx = sift_vexpr_ctx_new(cx);
	for (int k = 0; k < spec->nbins; k++)
	{
		fd->bin[k] = (SiftDateBin) {.stride_us = spec->bin[k].stride_us,.origin = spec->bin[k].origin,.stride_status = SDB_OK};
		fd->binout[k] = MemoryContextAllocAligned(cx, SIFT_VEC_LEN * sizeof(int64) + SIFT_VEC_PAD, SIFT_VEC_ALIGN, 0);
	}
	r->fd = fd;
	sift_stream_init(&r->se, (uint8) spec->nkeys, false, false, 0, SIFT_STREAM_NO_LIMIT, emit, NULL, emit_arg);
	sift_stream_add_source(&r->se, feed_next, fd);
	return r;
}

bool
sift_v6_stream_step(SiftV6StreamRun *r)
{
	return sift_stream_step(&r->se);
}

void
sift_v6_stream_close(SiftV6StreamRun *r)
{
	sift_stream_free(&r->se);
	pfree(r);
}

/* ---- the planner ---- */

typedef struct StreamPlan
{
	PlannerInfo *root;
	RelOptInfo *rel;
	RelOptInfo *out;
	const SiftV6RelSource *src;
	GroupPathExtraData *extra;
	List	   *keys;
	List	   *aggs;
	List	   *scan_tlist;
	List	   *quals;
} StreamPlan;

static bool
keys_of(StreamPlan *p)
{
	const Query *q = p->root->parse;

	foreach_node(SortGroupClause, sgc, p->root->processed_groupClause)
	{
		Node	   *e = (Node *) get_sortgroupclause_expr(sgc, q->targetList);
		bool		desc;

		if ((!IsA(e, Var) && !IsA(e, FuncExpr)) || !OidIsValid(get_equality_op_for_ordering_op(sgc->sortop, &desc)) || desc || sgc->nulls_first)
			return false;
		p->keys = lappend(p->keys, e);
	}
	return p->keys != NIL;
}

static void
tlist_add(StreamPlan *p, Expr *e)
{
	foreach_node(TargetEntry, te, p->scan_tlist)
		if (equal(te->expr, e))
			return;
	p->scan_tlist = lappend(p->scan_tlist, makeTargetEntry(e, list_length(p->scan_tlist) + 1, NULL, false));
}

/* The entries the node emits: the keys and the aggregates of the target and of HAVING, and nothing else */
static bool
tlist_of(StreamPlan *p)
{
	List	   *having = p->extra->havingQual != NULL ? sift_v6_scan_tlist(list_make1(p->extra->havingQual), p->rel->relid, T_Aggref) : NIL;

	foreach_ptr(Expr, e, p->out->reltarget->exprs)
	{
		if (!IsA(e, Aggref) && !list_member(p->keys, e))
			return false;
		tlist_add(p, e);
	}
	foreach_node(TargetEntry, te, having)
	{
		if (!IsA(te->expr, Aggref) && !list_member(p->keys, te->expr))
			return false;
		tlist_add(p, te->expr);
	}
	foreach_node(TargetEntry, te, p->scan_tlist)
		if (IsA(te->expr, Aggref))
			p->aggs = lappend(p->aggs, te->expr);
	return p->aggs != NIL;
}

static bool
collect(StreamPlan *p)
{
	if (!keys_of(p) || !tlist_of(p))
		return false;
	foreach_node(RestrictInfo, ri, p->rel->baserestrictinfo)
	{
		if (ri->pseudoconstant)
			return false;
		p->quals = lappend(p->quals, ri->clause);
	}
	return true;
}

static const CustomPathMethods stream_path_methods = {"SiftAggV6Stream", sift_agg_v6_stream_plan_path, NULL};

static bool
stream_try(StreamPlan *p)
{
	SiftV6Input in = p->src->input;
	SiftV6StreamSpec spec;
	SiftV6Vexpr probe,
				filters[SIFT_V6_STREAM_MAX_FILTERS];
	List	   *priv;
	int			atts[SIFT_STREAM_MAX_KEYS];
	double		groups;
	Cost		open,
				scan;
	const SiftCardStatsView *view = NULL;

	if (!collect(p))
		return false;
	priv = list_make5(list_make3_int((int) p->src->reloid, (int) p->rel->relid, p->src->is_table), p->scan_tlist, p->keys, p->aggs, (List *) p->extra->havingQual);
	priv = lappend(priv, p->quals);
	if (!sift_v6_stream_spec(priv, &in, &spec) || !sift_v6_stream_filters(CurrentMemoryContext, &spec, NULL, &in, filters) ||
		!sift_v6_vexpr_quals(CurrentMemoryContext, p->quals, p->rel->relid, NULL, &in, &probe))
		return false;
	for (int i = 0; i < spec.nkeys; i++)
		atts[i] = in.col[spec.key_slot[i]].attno;
	if (!sift_v6_rel_sorted_by(p->src->reloid, p->src->is_table, atts, spec.nkeys))
		return false;
	if (!p->src->is_table)
		view = sift_v6_rel_card_view(p->src->reloid);
	groups = sift_v6_card_groups(p->root, p->rel, p->src->ix, view, p->keys, p->rel->rows);
	if (groups < 0)
		groups = estimate_num_groups(p->root, p->keys, p->rel->rows, NULL, NULL);
	sift_v6_source_scan_price(p->root, p->rel, p->src, 0, &open, &scan);
	scan += cpu_tuple_cost * groups;
	sift_v6_add_path(p->out, &stream_path_methods, open + (scan - open) / Max(groups, 1.0), scan, groups, p->root->group_pathkeys, priv, NIL);
	return true;
}

void
sift_agg_v6_stream_upper_paths(PlannerInfo *root, UpperRelationKind stage, RelOptInfo *input_rel, RelOptInfo *output_rel, void *extra)
{
	const Query *q = root->parse;
	SiftV6RelSource *src;
	int			nsrc;

	if (stage != UPPERREL_GROUP_AGG || !sift_enable_agg || q->hasTargetSRFs || q->groupingSets != NIL ||
		root->processed_groupClause == NIL || !q->hasAggs || !sift_v6_base_rel_ok(root, input_rel))
		return;
	sift_v6_stage_target(root, stage, output_rel);
	src = sift_v6_rel_sources_alloc(root, input_rel, &nsrc);
	for (int i = 0; i < nsrc; i++)
	{
		StreamPlan	p = {.root = root,.rel = input_rel,.out = output_rel,.src = &src[i],.extra = extra};

		if (sift_v6_source_usable(&src[i]) && sift_v6_rel_ordered(src[i].reloid, src[i].is_table) && stream_try(&p))
			return;
	}
}

Plan *
sift_agg_v6_stream_plan_path(PlannerInfo *root, RelOptInfo *rel, CustomPath *best, List *tlist, List *clauses, List *custom_plans)
{
	const List *priv = best->custom_private;
	Plan	   *plan = sift_v6_plan_scan(root, best, tlist, &sift_agg_v6_stream_plan_methods, (Oid) linitial_int(linitial(priv)),
										 (Node *) list_make3(list_nth(priv, PRIV_KEYS), list_nth(priv, PRIV_AGGS), list_nth(priv, PRIV_QUALS)));

	plan->qual = list_nth(best->custom_private, PRIV_HAVING);
	return plan;
}

/* ---- the node ---- */

typedef struct StreamState
{
	CustomScanState css;
	List	   *priv;
	SiftV6StreamSpec spec;
	MemoryContext cx;			/* one run of the engine */
	SiftV6Source *src;
	SiftV6StreamRun *run;
	SiftV6Fifo	fifo;
	int		   *entry;			/* of each entry of the scan tlist: its key (0 ..) or -1 - its aggregate */
	Datum	   *vals;
	bool	   *nulls;
	uint64		groups;
	bool		started;
	bool		done;
} StreamState;

static void
stream_begin(CustomScanState *node, EState *estate, int eflags)
{
	StreamState *st = (StreamState *) node;
	const CustomScan *cs = (CustomScan *) node->ss.ps.plan;
	const List *tlist = list_nth(cs->custom_private, PRIV_TLIST);
	int			i = 0;

	st->priv = cs->custom_private;
	st->entry = palloc(list_length(tlist) * sizeof(int));
	st->vals = palloc(list_length(tlist) * sizeof(Datum));
	st->nulls = palloc(list_length(tlist) * sizeof(bool));
	sift_v6_fifo_open(&st->fifo, estate->es_query_cxt);
	foreach_node(TargetEntry, te, (List *) tlist)
	{
		int			j = 0;

		st->entry[i] = 0;
		if (IsA(te->expr, Aggref))
			foreach_ptr(Node, a, list_nth(cs->custom_private, PRIV_AGGS))
			{
				if (equal(a, te->expr))
				{
					st->entry[i] = -1 - j;
					break;
				}
				j++;
			}
		else
			foreach_ptr(Node, k, list_nth(cs->custom_private, PRIV_KEYS))
			{
				if (equal(k, te->expr))
				{
					st->entry[i] = j;
					break;
				}
				j++;
			}
		i++;
	}
}

static void
stream_emit(void *arg, const SiftStreamGroup *g)
{
	StreamState *st = arg;
	const int	n = list_length(list_nth(st->priv, PRIV_TLIST));
	MemoryContext old = MemoryContextSwitchTo(st->fifo.row_cx);

	for (int i = 0; i < n; i++)
	{
		const int	e = st->entry[i];

		if (e >= 0)
		{
			st->nulls[i] = (g->key_nulls >> e) & 1;
			st->vals[i] = st->nulls[i] ? (Datum) 0 : sift_v6_stream_key_datum(&st->spec, e, g);
		}
		else
			st->nulls[i] = !sift_v6_stream_datum(&st->spec, -1 - e, g, &st->vals[i]);
	}
	MemoryContextSwitchTo(old);
	sift_v6_fifo_put(&st->fifo, st->css.ss.ss_ScanTupleSlot->tts_tupleDescriptor, st->vals, st->nulls);
	st->groups++;
}

static void
stream_start(StreamState *st, EState *estate)
{
	SiftV6Input in;
	SiftV6Vexpr quals,
				filters[SIFT_V6_STREAM_MAX_FILTERS];
	bool		compiled;
	List	   *where = list_nth(st->priv, PRIV_QUALS);
	const bool	is_table = list_nth_int(linitial(st->priv), 2);
	Relation	source = sift_v6_source_open((Oid) linitial_int(linitial(st->priv)), is_table, AccessShareLock, &in);

	sift_v6_source_enter(source, is_table, estate->es_snapshot);
	st->cx = sift_mem_allocset_create(estate->es_query_cxt, "sift agg v6 run");
	sift_v6_input_bind(&in, estate);
	sift_v6_source_close(source, is_table, NoLock);
	compiled = sift_v6_stream_spec(st->priv, &in, &st->spec) && sift_v6_stream_filters(st->cx, &st->spec, estate->es_param_list_info, &in, filters) &&
		sift_v6_vexpr_quals(st->cx, where, st->spec.rti, estate->es_param_list_info, &in, &quals);
	SIFT_INVARIANT(compiled, "the stream of groups of relation %u is not compiled", st->spec.indexoid);
	st->src = sift_v6_rel_open(estate, &in, where == NIL ? NULL : &quals, true);
	st->run = sift_v6_stream_open(st->cx, st->src, &st->spec, filters, stream_emit, st);
	st->groups = 0;
	st->started = true;
}

static TupleTableSlot *
stream_next(ScanState *ss)
{
	StreamState *st = (StreamState *) ss;

	if (!st->started)
		stream_start(st, ss->ps.state);
	for (;;)
	{
		TupleTableSlot *slot = sift_v6_fifo_get(&st->fifo, ss->ss_ScanTupleSlot);

		if (slot != NULL || st->done)
			return slot;
		st->done = !sift_v6_stream_step(st->run);
	}
}

static bool
stream_recheck(ScanState *ss, TupleTableSlot *slot)
{
	return true;
}

static TupleTableSlot *
stream_exec(CustomScanState *node)
{
	return ExecScan(&node->ss, stream_next, stream_recheck);
}

static void
stream_release(StreamState *st)
{
	if (st->run != NULL)
		sift_v6_stream_close(st->run);
	if (st->src != NULL)
		st->src->close(st->src);
	if (st->cx != NULL)
		MemoryContextDelete(st->cx);
	st->run = NULL;
	st->src = NULL;
	st->cx = NULL;
	st->started = st->done = false;
}

static void
stream_end(CustomScanState *node)
{
	StreamState *st = (StreamState *) node;

	stream_release(st);
	sift_v6_fifo_close(&st->fifo);
}

/* a scan again reads the input again: no tuple of the run is kept to be given twice */
static void
stream_rescan(CustomScanState *node)
{
	StreamState *st = (StreamState *) node;

	stream_release(st);
	sift_v6_fifo_clear(&st->fifo);
}

static void
stream_explain(CustomScanState *node, List *ancestors, ExplainState *es)
{
	StreamState *st = (StreamState *) node;

	sift_v6_explain_index(es, (Oid) linitial_int(linitial(st->priv)), list_nth_int(linitial(st->priv), 2));
	if (st->started)
		sift_v6_explain_count(es, "Groups", (int64) st->groups);
}

static const CustomExecMethods stream_exec_methods = {
	.CustomName = "SiftAggV6Stream",
	.BeginCustomScan = stream_begin,
	.ExecCustomScan = stream_exec,
	.EndCustomScan = stream_end,
	.ReScanCustomScan = stream_rescan,
	.ExplainCustomScan = stream_explain,
};

static Node *
stream_create_state(CustomScan *cscan)
{
	StreamState *st = (StreamState *) newNode(sizeof(StreamState), T_CustomScanState);

	st->css.methods = &stream_exec_methods;
	st->css.slotOps = &TTSOpsMinimalTuple;
	return (Node *) st;
}

const CustomScanMethods sift_agg_v6_stream_plan_methods = {"SiftAggV6Stream", stream_create_state};
