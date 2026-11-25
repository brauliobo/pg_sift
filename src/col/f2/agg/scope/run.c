/*
 * col/f2/agg/scope/run.c - the executor of the scope aggregate of SiftAggV6 (planned by scope_v6.c): the cube of each key the statement reads, or of each month of it, as the tuples of the scan.
 * The summary, with the rows of the parts it does not count read as the snapshot sees them, answers only when the index has heard of every change the snapshot sees (sift_f2_scope_open_keys) and the cubes can say what the statement asks
 * (months are those of UTC: a timestamptz is cut into them only where the session writes its times in UTC); when they cannot, the node runs the aggregate over the join as the planner priced it,
 * which the path holds as its child.
 */
/* check-memory: allow-allocs every array here follows the aggregates of one statement and the keys of one summary */
#include "postgres.h"

#include "access/genam.h"
#include "catalog/pg_type.h"
#include "utils/rel.h"
#include "access/relation.h"
#include "access/skey.h"
#include "access/stratnum.h"
#include "access/table.h"
#include "access/tableam.h"
#include "commands/explain.h"
#include "executor/executor.h"
#include "executor/nodeIndexscan.h"
#include "miscadmin.h"
#include "nodes/nodeFuncs.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/date.h"
#include "utils/fmgrprotos.h"
#include "utils/lsyscache.h"
#include "utils/numeric.h"
#include "utils/timestamp.h"

#include "agg/agg.h"
#include "col/f2/agg/scan.h"
#include "col/f2/agg/scope/int.h"
#include "core/zoneoff.h"
#include "col/am.h"
#include "col/col.h"
#include "col/f2/scope/scope.h"
#include "col/f2/scope/route.h"
#include "core/page_io.h"
#include "core/cmp.h"
#include "core/memtrack.h"
#include "core/compat.h"

typedef struct ScopeState
{
	CustomScanState css;
	ScopeSpec  *spec;
	Relation	rel;			/* the index that keeps the summary, or the sift_table */
	ExprState  *key;
	Oid			key_type;
	SiftKeyMap *km;				/* the codes of a text, varchar or uuid key of the scope (the statement's own map), NULL for an integer key */
	ExprState  *lo;
	ExprState  *hi;
	MemoryContext rcx;
	bool		started;
	bool		from_summary;
	Datum	  **vals;
	bool	  **nulls;
	int			nrows;
	int			cap_rows;
	int			at;
	uint64		keys_read;
	SiftV6Spec *v6;				/* a scan of the facts (spec->scan): its spec, its keys and groups */
	Relation	heap;
	ScanKey		scan_keys;
	int			nscan_keys;
	MemoryContext gcx;
	SiftV6Group *group;
	const SiftScopeRouter *router;	/* scan: the granules of the key that the summary names, NULL when it cannot route */
	SiftV6ScanStats stats;
	int64		members;
	bool		inexact;		/* a minimum or maximum cannot be kept after the pair edits: the join answers */
	SiftScopeServeMode mode;	/* how the summary served the read */
} ScopeState;

static void
scope_begin(CustomScanState *node, EState *estate, int eflags)
{
	ScopeState *st = (ScopeState *) node;
	CustomScan *cs = (CustomScan *) node->ss.ps.plan;

	st->spec = sift_v6_scope_spec_unpack(linitial(cs->custom_private));
	st->rel = st->spec->is_table ? table_open(st->spec->relid, AccessShareLock) : index_open(st->spec->relid, AccessShareLock);
	st->rcx = sift_mem_allocset_create(estate->es_query_cxt, "sift agg v6 scope rows");
	if (sift_keymap_type(st->spec->key_type))
		st->km = sift_f2_scope_keymap(st->rel, st->spec->ord);
	foreach_ptr(Plan, child, cs->custom_plans)
		node->custom_ps = lappend(node->custom_ps, ExecInitNode(child, estate, eflags));
	if (st->spec->key_given)
	{
		st->key = ExecInitExpr(linitial(cs->custom_exprs), &node->ss.ps);
		st->key_type = st->spec->key_array ? get_element_type(exprType(linitial(cs->custom_exprs))) : exprType(linitial(cs->custom_exprs));
	}
	if (st->spec->lo_strat != 0)
		st->lo = ExecInitExpr(list_nth(cs->custom_exprs, st->spec->key_given), &node->ss.ps);
	if (st->spec->hi_strat != 0)
		st->hi = ExecInitExpr(list_nth(cs->custom_exprs, st->spec->key_given + (st->lo != NULL)), &node->ss.ps);
	if (st->spec->scan || st->spec->topk)
		st->v6 = sift_v6_spec_unpack(lsecond(cs->custom_private));
	if (st->spec->scan)
	{
		IndexRuntimeKeyInfo *rtkeys = NULL;
		int			nrt = 0;

		st->heap = table_open(st->rel->rd_index->indrelid, AccessShareLock);
		ExecIndexBuildScanKeys(&node->ss.ps, st->rel, st->v6->indexqual, false, &st->scan_keys, &st->nscan_keys, &rtkeys, &nrt, NULL, NULL);
		SIFT_INVARIANT(nrt == 0, "the quals of the aggregate scan are constants");
		sift_v6_json_scankeys(st->v6, st->rel, &st->scan_keys, &st->nscan_keys);
	}
}

static int64
key_value(ScopeState *st, bool *isnull)
{
	ExprContext *ec = st->css.ss.ps.ps_ExprContext;
	Datum		d = ExecEvalExprSwitchContext(st->key, ec, isnull);

	return *isnull ? 0 : sift_scope_key_code(st->key_type, st->km, d);
}

/* More keys than this are the join's: each costs a cube */
#define SCOPE_KEYS_MAX	256

/* The scope keys the statement names, ascending and distinct (those of its array, the one key, or none for a NULL), in *keys; false when they are more than a read is worth */
static bool
key_list(ScopeState *st, int64 **keys, uint32 *nkeys)
{
	bool		isnull = true;
	Datum		d = st->spec->key_given ? ExecEvalExprSwitchContext(st->key, st->css.ss.ps.ps_ExprContext, &isnull) : (Datum) 0;
	Datum	   *vals;
	bool	   *nulls;
	int			n = 0;
	uint32		u = 0;
	int16		typlen;
	bool		typbyval;
	char		typalign;

	*keys = palloc(sizeof(int64));
	*nkeys = 0;
	if (isnull)
		return true;
	if (!st->spec->key_array)
	{
		(*keys)[(*nkeys)++] = sift_scope_key_code(st->key_type, st->km, d);
		return true;
	}
	get_typlenbyvalalign(st->key_type, &typlen, &typbyval, &typalign);
	deconstruct_array(DatumGetArrayTypeP(d), st->key_type, typlen, typbyval, typalign, &vals, &nulls, &n);
	if (n > SCOPE_KEYS_MAX)
		return false;
	*keys = palloc(Max(n, 1) * sizeof(int64));
	for (int i = 0; i < n; i++)
		if (!nulls[i])
			(*keys)[u++] = sift_scope_key_code(st->key_type, st->km, vals[i]);
	qsort(*keys, u, sizeof(int64), sift_cmp_i64);
	for (uint32 i = 0; i < u; i++)
		if (*nkeys == 0 || (*keys)[*nkeys - 1] != (*keys)[i])
			(*keys)[(*nkeys)++] = (*keys)[i];
	return true;
}

static Datum
int_datum(Oid typid, int64 v)
{
	return typid == INT2OID ? Int16GetDatum((int16) v) : typid == INT4OID ? Int32GetDatum((int32) v) : Int64GetDatum(v);
}

static SiftI128
i128_of(SiftCI128 v)
{
	return (SiftI128) {.lo = (uint64) v,.hi = (int64) (v >> 64)};
}

static Datum
sum_datum(const SiftV6Agg *a, SiftCI128 sum)
{
	if (a->restype == INT8OID && (sum > PG_INT64_MAX || sum < PG_INT64_MIN))
		ereport(ERROR, (errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE), errmsg("bigint out of range")));
	return a->restype == INT8OID ? Int64GetDatum((int64) sum) : sift_v6_scaled_numeric(i128_of(sum), a->arg.scale);
}

static double
bits_double(int64 bits)
{
	double		d;

	memcpy(&d, &bits, sizeof(d));
	return d;
}

static Datum
float_datum(const SiftV6Agg *a, const SiftScopeLeafAgg *l, uint64 nn)
{
	switch (a->kind)
	{
		case SV6_SUM:
			return Float8GetDatum(l->sum_f);
		case SV6_AVG:
			return Float8GetDatum(l->sum_f / (double) nn);
		case SV6_MIN:
			return Float8GetDatum(bits_double(l->min));
		default:
			return Float8GetDatum(bits_double(l->max));
	}
}

static Datum
agg_datum(const SiftV6Agg *a, const SiftScopeCubeAcc *c, bool *isnull)
{
	const SiftScopeLeafAgg *l = a->kind == SV6_COUNT_STAR ? NULL : &c->leaf[a->arg.attno];
	const uint64 nn = l == NULL ? c->row_count : c->row_count - l->nnull;

	*isnull = false;
	if (a->kind == SV6_COUNT_STAR || a->kind == SV6_COUNT)
		return Int64GetDatum((int64) nn);
	if (nn == 0)
	{
		*isnull = true;
		return (Datum) 0;
	}
	if (a->arg.typid == FLOAT8OID)
		return float_datum(a, l, nn);
	switch (a->kind)
	{
		case SV6_SUM:
			return sum_datum(a, l->sum);
		case SV6_AVG:
			return DirectFunctionCall2(numeric_div, sift_v6_scaled_numeric(i128_of(l->sum), a->arg.scale), NumericGetDatum(int64_to_numeric((int64) nn)));
		default:
		{
			const int64 v = a->kind == SV6_MIN ? l->min : l->max;

			return a->arg.typid == NUMERICOID ? sift_v6_scaled_numeric((SiftI128) {.lo = (uint64) v,.hi = v < 0 ? -1 : 0}, a->arg.scale) : int_datum(a->arg.typid, v);
		}
	}
}

static Datum
key_datum(const ScopeState *st, int64 key)
{
	return sift_scope_key_datum(st->spec->key_type, st->km, key);
}

/* A tuple: the key, the month and the aggregates of the cube, in the order of the scan tlist */
static void
add_row(ScopeState *st, const SiftScopeCubeAcc *c, int64 key, int32 month)
{
	const ScopeSpec *sp = st->spec;
	/* MEM[MEM-SCOPE-11]: scope_v6_run materialized rows (2 pallocs/row in st->rcx) */
	Datum	   *v = palloc(Max(sp->ncols, 1) * sizeof(Datum));
	bool	   *n = palloc0(Max(sp->ncols, 1) * sizeof(bool));

	for (int i = 0; i < sp->ncols; i++)
		v[i] = sp->col[i] == SCOPE_COL_KEY ? key_datum(st, key) : sp->col[i] == SCOPE_COL_MONTH ? TimestampTzGetDatum(sift_scope_month_start(month)) : agg_datum(&sp->agg[sp->col[i]], c, &n[i]);
	if (st->nrows == st->cap_rows)
	{
		st->cap_rows = st->cap_rows == 0 ? 64 : st->cap_rows * 2;
		st->vals = st->vals ? repalloc(st->vals, (Size) st->cap_rows * sizeof(Datum *)) : palloc((Size) st->cap_rows * sizeof(Datum *));
		st->nulls = st->nulls ? repalloc(st->nulls, (Size) st->cap_rows * sizeof(bool *)) : palloc((Size) st->cap_rows * sizeof(bool *));
	}
	st->vals[st->nrows] = v;
	st->nulls[st->nrows++] = n;
}

static bool
needs_minmax(const ScopeSpec *sp)
{
	for (int j = 0; j < sp->naggs; j++)
		if (sp->agg[j].kind == SV6_MIN || sp->agg[j].kind == SV6_MAX)
			return true;
	return false;
}

/* The window [lo, hi) of the statement and the months it touches: those it holds whole are read from the cubes, the (at most two) it cuts from the rows of the key; not more months than a cube read is worth.
 * A bound the statement does not state is open: only a statement that groups by month has one, and it reads the months the key has */
typedef struct MonthWindow
{
	bool		ranged;
	int64		lo;
	int64		hi;
	int32		first;
	int32		last;
} MonthWindow;

#define MONTH_WINDOW_MAX	600

static bool
bound_micros(ScopeState *st, ExprState *e, int strat, bool upper, int64 *out)
{
	bool		isnull;
	Datum		d = ExecEvalExprSwitchContext(e, st->css.ss.ps.ps_ExprContext, &isnull);
	const bool	date = st->spec->time_type == DATEOID;
	const int64 unit = date ? USECS_PER_DAY : 1;
	const bool	inclusive = strat == (upper ? BTLessEqualStrategyNumber : BTGreaterStrategyNumber);
	int64		v = date ? (int64) DatumGetDateADT(d) : DatumGetInt64(d);

	if (isnull || (date ? DATE_NOT_FINITE(DatumGetDateADT(d)) : TIMESTAMP_NOT_FINITE(DatumGetInt64(d))))
		return false;
	*out = v * unit + (inclusive ? unit : 0);
	return true;
}

static bool
month_window(ScopeState *st, MonthWindow *w)
{
	*w = (MonthWindow) {.lo = PG_INT64_MIN,.hi = PG_INT64_MAX,.first = PG_INT32_MIN,.last = PG_INT32_MAX};
	if (!st->spec->ranged)
		return true;
	if ((st->lo != NULL && !bound_micros(st, st->lo, st->spec->lo_strat, false, &w->lo)) || (st->hi != NULL && !bound_micros(st, st->hi, st->spec->hi_strat, true, &w->hi)))
		return false;
	w->ranged = true;
	if (st->lo != NULL)
		w->first = sift_scope_month_of(w->lo);
	if (st->hi != NULL)
		w->last = w->lo < w->hi ? sift_scope_month_of(w->hi - 1) : w->first - 1;
	return st->spec->by_month || (int64) w->last - w->first < MONTH_WINDOW_MAX;
}

static bool
month_whole(const MonthWindow *w, int32 month)
{
	return w->lo <= sift_scope_month_start(month) && sift_scope_month_end(month) <= w->hi;
}

/* The cube of the key (of the month q names) as the snapshot sees it: with the pairs edited the entity side tells what the summary lacks or counts too many */
static bool
cube_at(ScopeState *st, SiftF2Scope *sc, const SiftScopeL2Root *root, int64 key, const SiftScopeQueryNeeds *q, SiftScopeCubeAcc *out)
{
	SiftScopeDeltaStats stats = {0};
	bool		exact;
	bool		has;

	if (sc->nchanged == 0)
		return root != NULL && sift_scope_cube_live(&sc->meta, root, q, &sc->entity, out, &stats);
	has = sift_f2_scope_key_cube(sc, key, q, out, &exact);
	st->inexact |= !exact;
	return has;
}

/* The rows of the key in the month that the window holds: the cube of the month when it holds all of it, else the rows of the part the window cuts */
static bool
window_month_cube(ScopeState *st, SiftF2Scope *sc, const SiftScopeL2Root *root, int64 key, SiftScopeQueryNeeds q, const MonthWindow *w, int32 month, SiftScopeCubeAcc *out)
{
	if (w->ranged && !month_whole(w, month))
		return sift_f2_scope_window_cube(sc, key, &q, Max(w->lo, sift_scope_month_start(month)), Min(w->hi, sift_scope_month_end(month)), out);
	q.month = month;
	return cube_at(st, sc, root, key, &q, out);
}

static bool
read_cube(ScopeState *st, SiftF2Scope *sc, const SiftScopeL2Root *root, int64 key, SiftScopeQueryNeeds q, const MonthWindow *w, SiftScopeCubeAcc *out)
{
	bool		any = false;

	if (!w->ranged)
		return cube_at(st, sc, root, key, &q, out);
	for (int32 m = w->first; m <= w->last; m++)
	{
		SiftScopeCubeAcc c;

		if (!window_month_cube(st, sc, root, key, q, w, m, any ? &c : out))
			continue;
		if (any)
		{
			sift_scube_merge(out, &c, NULL);
			sift_scube_free(&c);
		}
		any = true;
	}
	return any;
}

/* A tuple for each month the key has rows in: false when rows of it are in no month (their time is NULL: the groups would need a month that is NULL) */
static bool
read_months(ScopeState *st, SiftF2Scope *sc, const SiftScopeL2Root *root, SiftScopeQueryNeeds q, const MonthWindow *w)
{
	int32	   *months;
	const uint32 nm = sift_scope_months_live(&sc->meta, root, &sc->entity, &months);
	SiftScopeDeltaStats stats = {0};
	uint64		in_months = 0;
	SiftScopeCubeAcc all;

	for (uint32 i = 0; i < nm; i++)
	{
		SiftScopeCubeAcc c;

		if (w->ranged && (months[i] < w->first || months[i] > w->last))
			continue;
		if (!window_month_cube(st, sc, root, root->scope_key, q, w, months[i], &c))
			continue;
		in_months += c.row_count;
		add_row(st, &c, root->scope_key, months[i]);
		sift_scube_free(&c);
	}
	pfree(months);
	if (w->ranged)
		return true;
	q.month = SIFT_SCOPE_NO_MONTH;
	if (!sift_scope_cube_live(&sc->meta, root, &q, &sc->entity, &all, &stats))
		return in_months == 0;
	in_months = all.row_count == in_months;
	sift_scube_free(&all);
	return in_months;
}

static void
read_key(ScopeState *st, SiftF2Scope *sc, const SiftScopeL2Root *root, int64 key, const SiftScopeQueryNeeds *q, const MonthWindow *w)
{
	SiftScopeCubeAcc cube;

	st->keys_read++;
	if (read_cube(st, sc, root, key, *q, w, &cube))
	{
		add_row(st, &cube, key, 0);
		sift_scube_free(&cube);
	}
	else if (!st->spec->by_key && !st->spec->grouped_const)
	{
		SiftScopeCubeAcc none = {0};

		add_row(st, &none, 0, 0);
	}
}

/* The cubes of the keys added, for a statement that names several keys and does not group by them: the aggregates of the join over all of them (a row of the facts counts once for each key its member is in) */
static bool
read_merged(ScopeState *st, SiftF2Scope *sc, const int64 *keys, uint32 nkeys, const SiftScopeQueryNeeds *q, const MonthWindow *w)
{
	SiftScopeCubeAcc all;
	bool		any = false;

	for (uint32 i = 0; i < nkeys; i++)
	{
		SiftScopeCubeAcc c;

		st->keys_read++;
		if (!read_cube(st, sc, sift_scope_l2_find(&sc->meta, keys[i]), keys[i], *q, w, &c))
			continue;
		if (any)
		{
			sift_scube_merge(&all, &c, NULL);
			sift_scube_free(&c);
		}
		else
			all = c;
		any = true;
	}
	if (any)
	{
		add_row(st, &all, 0, 0);
		sift_scube_free(&all);
	}
	else if (!st->spec->grouped_const)
	{
		SiftScopeCubeAcc none = {0};

		add_row(st, &none, 0, 0);
	}
	return !st->inexact;
}

static bool
read_summary(ScopeState *st, SiftF2Scope *sc, const MonthWindow *w, const int64 *keys, uint32 nkeys)
{
	const SiftScopeQueryNeeds q = {.minmax = needs_minmax(st->spec),.month = SIFT_SCOPE_NO_MONTH};
	bool		ok = true;

	if (st->spec->key_array && !st->spec->by_key)
		return read_merged(st, sc, keys, nkeys, &q, w);
	for (uint32 i = 0; i < (st->spec->key_given ? Max(nkeys, 1) : sc->meta.hdr.nkeys); i++)
	{
		const int64 key = st->spec->key_given && i < nkeys ? keys[i] : 0;
		const SiftScopeL2Root *root = !st->spec->key_given ? &sc->meta.roots[i] : i < nkeys ? sift_scope_l2_find(&sc->meta, key) : NULL;

		if (st->spec->by_month && st->spec->key_given && root == NULL)
			continue;
		if (st->spec->by_month)
			ok = ok && read_months(st, sc, root, q, w);
		else
			read_key(st, sc, root, st->spec->key_given ? key : root->scope_key, &q, w);
	}
	return ok && !st->inexact;
}

/* The months of the cubes are those of UTC: a statement that cuts a timestamptz into the months of another zone is the join's */
static bool
months_exact(const ScopeState *st)
{
	return !st->spec->by_month || sift_scope_monthmap_tz_safe(st->spec->month_type == TIMESTAMPTZOID, zone_is_utc(session_timezone));
}

/* The members of the pairs of the key K, as the snapshot sees them: the array the scan of the facts takes as the list of the member column */
static ArrayType *
key_members(ScopeState *st, EState *estate)
{
	const Oid	member_type = TupleDescAttr(RelationGetDescr(st->rel), st->spec->member_col - 1)->atttypid;
	bool		no_key;
	const int64 key = key_value(st, &no_key);
	int64	   *members = NULL;
	const uint32 n = no_key ? 0 : sift_f2s_key_members(st->heap, st->rel, st->spec->ord, estate->es_snapshot, key, &members);
	Datum	   *vals = palloc(Max(n, 1) * sizeof(Datum));

	sift_budget_check((Size) n * sizeof(Datum), work_mem, "the members of a scope key", "work_mem", NULL);
	for (uint32 i = 0; i < n; i++)
		vals[i] = int_datum(member_type, members[i]);
	st->members = n;
	return construct_array_builtin(vals, n, member_type);
}

/* The granules of the key K that the summary names, which hold all its facts; NULL for a NULL key or a summary that cannot route */
static const SiftScopeRouter *
scan_router(ScopeState *st, EState *estate)
{
	bool		no_key;
	const int64 key = key_value(st, &no_key);

	return no_key ? NULL : sift_f2_scope_build_router(st->rel, st->spec->ord, estate->es_snapshot, &key, 1, st->km, st->gcx);
}

/* The facts of the key K are the facts whose member is one of its members: the scan of the index with the list among its keys, in the granules of the key */
static void
scan_run(ScopeState *st, EState *estate)
{
	MemoryContext old = MemoryContextSwitchTo(estate->es_query_cxt);
	ScanKey		keys = palloc((st->nscan_keys + 1) * sizeof(ScanKeyData));

	st->started = true;
	st->gcx = sift_mem_allocset_create(estate->es_query_cxt, "sift agg v6 scope groups");
	memcpy(keys, st->scan_keys, st->nscan_keys * sizeof(ScanKeyData));
	keys[st->nscan_keys] = (ScanKeyData) {.sk_flags = SK_SEARCHARRAY,.sk_attno = st->spec->member_col,.sk_strategy = BTEqualStrategyNumber,
	.sk_argument = PointerGetDatum(key_members(st, estate))};
	st->router = scan_router(st, estate);
	st->group = sift_v6_group_new(st->v6, st->gcx, st->css.ss.ss_ScanTupleSlot->tts_tupleDescriptor);
	sift_v6_scan_run(st->v6, st->group, st->heap, st->rel, keys, st->nscan_keys + 1, estate->es_snapshot, NULL, 0, st->router, &st->stats, NULL, NULL);
	sift_v6_group_finish(st->group);
	MemoryContextSwitchTo(old);
}

static TupleTableSlot *
scan_next(ScanState *ss)
{
	ScopeState *st = (ScopeState *) ss;
	TupleTableSlot *slot = ss->ss_ScanTupleSlot;

	if (!st->started)
		scan_run(st, ss->ps.state);
	ExecClearTuple(slot);
	MemoryContextSwitchTo(ss->ps.ps_ExprContext->ecxt_per_tuple_memory);
	if (!sift_v6_group_next(st->group, slot->tts_values, slot->tts_isnull))
		return NULL;
	return ExecStoreVirtualTuple(slot);
}

static Datum
cell_datum(const SiftColDef *d, Oid typid, const SiftF2ScopeCell *c)
{
	switch (sift_v6_class(typid))
	{
		case SV6C_I32:
			return Int32GetDatum((int32) c->i);
		case SV6C_I64:
			return typid == NUMERICOID ? sift_v6_scaled_numeric((SiftI128) {.lo = (uint64) c->i,.hi = c->i < 0 ? -1 : 0}, d->scale) : Int64GetDatum(c->i);
		case SV6C_F64:
			return typid == FLOAT4OID ? Float4GetDatum((float4) c->f) : Float8GetDatum(c->f);
		case SV6C_BOOL:
			return BoolGetDatum(c->i != 0);
		default:
			return PointerGetDatum(cstring_to_text_with_len(c->s, c->len));
	}
}

/* The cells of the newest rows as the tuples of the scan; false when a leaf holds no value of a row the heap has (the cell of an exception) */
static bool
topk_tuples(ScopeState *st, const SiftF2ScopeCell *cells, int n, const int *leaves)
{
	const int	nk = st->v6->nkeys;

	st->vals = palloc(Max(n, 1) * sizeof(Datum *));
	st->nulls = palloc(Max(n, 1) * sizeof(bool *));
	for (int k = 0; k < n; k++)
	{
		st->vals[k] = palloc(Max(nk, 1) * sizeof(Datum));
		st->nulls[k] = palloc(Max(nk, 1) * sizeof(bool));
		for (int j = 0; j < nk; j++)
		{
			const SiftF2ScopeCell *c = &cells[k * nk + j];

			if (c->isnull && c->tag != 0)
				return false;
			st->nulls[k][j] = c->isnull;
			st->vals[k][j] = c->isnull ? (Datum) 0 : cell_datum(&sift_col_rel(st->rel)->schema->cols[leaves[j]], st->v6->key[j].typid, c);
		}
	}
	st->nrows = st->cap_rows = n;
	return true;
}

/*
 * ORDER BY ring leaf DESC [, tie leaf] LIMIT n of the facts of the key: its ring and the rows the summary lacks, in the order of the statement (scope_v6_topn.c).
 */
static bool
topk_read(ScopeState *st, SiftF2Scope *sc)
{
	const ScopeSpec *sp = st->spec;
	const int	nk = st->v6->nkeys;
	int			leaves[SIFT_V6_MAX_KEYS],
				n;
	bool		no_key;
	const int64 key = key_value(st, &no_key);
	SiftF2ScopeCell *cells;

	for (int j = 0; j < nk; j++)
		leaves[j] = sift_col_rel(st->rel)->first[st->v6->key[j].attno];
	return sift_v6_scope_topn(sc, sp, key, no_key, sp->tie_col > 0 ? sift_col_rel(st->rel)->first[sp->tie_col] : 0, leaves, nk, &cells, &n) && topk_tuples(st, cells, n, leaves);
}

/* The summary for the statement's snapshot: of the keys it names the pairs may have been edited since; a read of every key, of months or of a window or ring answers only from pairs as they were */
static SiftF2Scope *
open_summary(ScopeState *st, EState *estate, const int64 *keys, uint32 nkeys)
{
	const ScopeSpec *sp = st->spec;
	SiftF2Scope *sc = sift_f2_scope_open_keys(st->rel, sp->ord, estate->es_snapshot, sp->excl_col, keys, nkeys, st->km);

	if (sc != NULL && sc->nchanged > 0 && sp->by_month)
	{
		sift_f2_scope_close(sc);
		return NULL;
	}
	return sc;
}

static void
scope_run(ScopeState *st, EState *estate)
{
	MemoryContext old = MemoryContextSwitchTo(st->rcx);
	SiftF2Scope *sc = NULL;
	MonthWindow w;
	int64	   *keys = NULL;
	uint32		nkeys = 0;

	st->started = true;
	if (months_exact(st) && month_window(st, &w) && key_list(st, &keys, &nkeys))
		sc = open_summary(st, estate, keys, nkeys);
	if (sc != NULL)
	{
		st->mode = sift_f2_scope_mode(sc);
		st->from_summary = st->spec->topk ? topk_read(st, sc) : read_summary(st, sc, &w, keys, nkeys);
		sift_f2_scope_close(sc);
		if (!st->from_summary)
		{
			st->nrows = st->cap_rows = st->at = 0;
			st->vals = NULL;
			st->nulls = NULL;
		}
	}
	MemoryContextSwitchTo(old);
}

static TupleTableSlot *
scope_next(ScanState *ss)
{
	ScopeState *st = (ScopeState *) ss;
	TupleTableSlot *slot = ss->ss_ScanTupleSlot;

	ExecClearTuple(slot);
	if (st->at >= st->nrows)
		return NULL;
	memcpy(slot->tts_values, st->vals[st->at], st->spec->ncols * sizeof(Datum));
	memcpy(slot->tts_isnull, st->nulls[st->at], st->spec->ncols * sizeof(bool));
	st->at++;
	return ExecStoreVirtualTuple(slot);
}

static bool
scope_recheck(ScanState *ss, TupleTableSlot *slot)
{
	return true;
}

/* The rows of the join are the node's own: the slot the child leaves its rows in is not the kind of slot the parents of this node were compiled against */
static TupleTableSlot *
join_next(ScanState *ss)
{
	TupleTableSlot *row = ExecProcNode((PlanState *) linitial(((CustomScanState *) ss)->custom_ps));

	return TupIsNull(row) ? NULL : ExecCopySlot(ss->ss_ScanTupleSlot, row);
}

static TupleTableSlot *
scope_exec(CustomScanState *node)
{
	ScopeState *st = (ScopeState *) node;
	MemoryContext old = CurrentMemoryContext;
	TupleTableSlot *slot;

	if (st->spec->scan)
	{
		slot = ExecScan(&node->ss, scan_next, scope_recheck);
		MemoryContextSwitchTo(old);
		return slot;
	}
	if (!st->started)
		scope_run(st, node->ss.ps.state);
	slot = ExecScan(&node->ss, st->from_summary ? scope_next : join_next, scope_recheck);
	MemoryContextSwitchTo(old);
	return slot;
}

static void
scan_reset(ScopeState *st)
{
	if (st->group != NULL)
		sift_v6_group_free(st->group);
	if (st->gcx != NULL)
		MemoryContextDelete(st->gcx);
	st->gcx = NULL;
	st->group = NULL;
	st->router = NULL;
	st->stats = (SiftV6ScanStats) {0};
}

static void
scope_reset(ScopeState *st)
{
	scan_reset(st);
	MemoryContextReset(st->rcx);
	st->vals = NULL;
	st->nulls = NULL;
	st->nrows = st->cap_rows = st->at = 0;
	st->started = st->from_summary = st->inexact = false;
	st->keys_read = 0;
}

static void
scope_end(CustomScanState *node)
{
	ScopeState *st = (ScopeState *) node;

	foreach_ptr(PlanState, child, node->custom_ps)
		ExecEndNode(child);
	scan_reset(st);
	MemoryContextDelete(st->rcx);
	if (st->heap != NULL)
		table_close(st->heap, AccessShareLock);
	relation_close(st->rel, AccessShareLock);
}

static void
scope_rescan(CustomScanState *node)
{
	scope_reset((ScopeState *) node);
	foreach_ptr(PlanState, child, node->custom_ps)
		ExecReScan(child);
}

static void
scope_explain(CustomScanState *node, List *ancestors, ExplainState *es)
{
	ScopeState *st = (ScopeState *) node;

	ExplainPropertyText(st->spec->is_table ? "Table" : "Index", RelationGetRelationName(st->rel), es);
	if (st->spec->scan)
	{
		ExplainPropertyText("Scope Read", "scan of the facts (the members of the key)", es);
		if (st->started)
		{
			ExplainPropertyInteger("Scope Members", NULL, st->members, es);
			if (st->router != NULL)
			{
				ExplainPropertyInteger("Scope Granules Kept", NULL, st->router->mask.kept, es);
				ExplainPropertyInteger("Scope Granules Total", NULL, st->router->mask.n, es);
			}
			ExplainPropertyUInteger("Rows Vetted Against The Heap", NULL, st->stats.heap_vet_rows, es);
		}
		return;
	}
	ExplainPropertyText("Scope Read", !st->started ? "summary or join" : st->from_summary ? (st->spec->topk ? "ring" : "summary") : "join (the summary is not exact for the snapshot)", es);
	if (st->started && st->from_summary && !st->spec->topk)
	{
		ExplainPropertyText("Scope Cube", st->mode == SIFT_SERVE_DELTA_CUBE ? "delta" : "clean", es);
		ExplainPropertyUInteger("Scope Keys Read", NULL, st->keys_read, es);
	}
}

static const CustomExecMethods scope_exec_methods = {
	.CustomName = "SiftAggV6Scope",
	.BeginCustomScan = scope_begin,
	.ExecCustomScan = scope_exec,
	.EndCustomScan = scope_end,
	.ReScanCustomScan = scope_rescan,
	.ExplainCustomScan = scope_explain,
};

static Node *
scope_create_state(CustomScan *cscan)
{
	ScopeState *st = (ScopeState *) newNode(sizeof(ScopeState), T_CustomScanState);

	st->css.methods = &scope_exec_methods;
	return (Node *) st;
}

const CustomScanMethods sift_agg_v6_scope_plan_methods = {"SiftAggV6Scope", scope_create_state};
