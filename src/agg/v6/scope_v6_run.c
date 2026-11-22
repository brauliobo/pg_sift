/*
 * agg/v6/scope_v6_run.c - the executor of the scope aggregate of SiftAggV6 (planned by scope_v6.c): the cube of each key the statement reads, or of each month of it, as the tuples of the scan.
 * The summary, with the rows of the parts it does not count read as the snapshot sees them, answers only when the index has heard of every change the snapshot sees (sift_f2_scope_open_keys) and the cubes can say what the statement asks
 * (months are those of UTC: a timestamptz is cut into them only where the session writes its times in UTC); when they cannot, the node runs the aggregate over the join as the planner priced it,
 * which the path holds as its child.
 */
/* check-memory: allow-allocs every array here follows the aggregates of one statement and the keys of one summary */
#include "postgres.h"

#include "access/genam.h"
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
#include "agg/v6/scope_v6_int.h"
#include "agg/zoneoff.h"
#include "col/am.h"
#include "col/col.h"
#include "col/f2/f2_scope.h"
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
	ExprState  *lo;
	ExprState  *hi;
	MemoryContext rcx;
	bool		started;
	bool		from_summary;
	Datum	  **vals;
	bool	  **nulls;
	int			nrows;
	int			at;
	uint64		keys_read;
	SiftV6Spec *v6;				/* a scan of the facts (spec->scan): its spec, its keys and groups */
	Relation	heap;
	ScanKey		scan_keys;
	int			nscan_keys;
	MemoryContext gcx;
	SiftV6Group *group;
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
	foreach_ptr(Plan, child, cs->custom_plans)
		node->custom_ps = lappend(node->custom_ps, ExecInitNode(child, estate, eflags));
	if (st->spec->key_given)
	{
		st->key = ExecInitExpr(linitial(cs->custom_exprs), &node->ss.ps);
		st->key_type = st->spec->key_array ? get_element_type(exprType(linitial(cs->custom_exprs))) : exprType(linitial(cs->custom_exprs));
	}
	if (st->spec->ranged)
	{
		st->lo = ExecInitExpr(list_nth(cs->custom_exprs, st->spec->key_given), &node->ss.ps);
		st->hi = ExecInitExpr(list_nth(cs->custom_exprs, st->spec->key_given + 1), &node->ss.ps);
	}
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

	return *isnull ? 0 : st->key_type == INT2OID ? (int64) DatumGetInt16(d) : st->key_type == INT4OID ? (int64) DatumGetInt32(d) : DatumGetInt64(d);
}

static int64
int_of(Oid typid, Datum d)
{
	return typid == INT2OID ? (int64) DatumGetInt16(d) : typid == INT4OID ? (int64) DatumGetInt32(d) : DatumGetInt64(d);
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

	*keys = palloc(sizeof(int64));
	*nkeys = 0;
	if (isnull)
		return true;
	if (!st->spec->key_array)
	{
		(*keys)[(*nkeys)++] = int_of(st->key_type, d);
		return true;
	}
	deconstruct_array_builtin(DatumGetArrayTypeP(d), st->key_type, &vals, &nulls, &n);
	if (n > SCOPE_KEYS_MAX)
		return false;
	*keys = palloc(Max(n, 1) * sizeof(int64));
	for (int i = 0; i < n; i++)
		if (!nulls[i])
			(*keys)[u++] = int_of(st->key_type, vals[i]);
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
key_datum(const ScopeSpec *sp, int64 key)
{
	return int_datum(sp->key_type, key);
}

/* A tuple: the key, the month and the aggregates of the cube, in the order of the scan tlist */
static void
add_row(ScopeState *st, const SiftScopeCubeAcc *c, int64 key, int32 month)
{
	const ScopeSpec *sp = st->spec;
	Datum	   *v = palloc(Max(sp->ncols, 1) * sizeof(Datum));
	bool	   *n = palloc0(Max(sp->ncols, 1) * sizeof(bool));

	for (int i = 0; i < sp->ncols; i++)
		v[i] = sp->col[i] == SCOPE_COL_KEY ? key_datum(sp, key) : sp->col[i] == SCOPE_COL_MONTH ? TimestampTzGetDatum(sift_scope_month_start(month)) : agg_datum(&sp->agg[sp->col[i]], c, &n[i]);
	st->vals = st->vals ? repalloc(st->vals, (st->nrows + 1) * sizeof(Datum *)) : palloc(sizeof(Datum *));
	st->nulls = st->nulls ? repalloc(st->nulls, (st->nrows + 1) * sizeof(bool *)) : palloc(sizeof(bool *));
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

/* The window of the statement as months of the cubes: its bounds are month starts of the leaf's own unit, and not more months than a cube read is worth */
typedef struct MonthWindow
{
	bool		ranged;
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
	int64		lo,
				hi;

	*w = (MonthWindow) {0};
	if (!st->spec->ranged)
		return true;
	if (!bound_micros(st, st->lo, st->spec->lo_strat, false, &lo) || !bound_micros(st, st->hi, st->spec->hi_strat, true, &hi))
		return false;
	w->ranged = true;
	w->first = sift_scope_month_of(lo);
	w->last = sift_scope_month_of(hi) - 1;
	return sift_scope_month_start(w->first) == lo && sift_scope_month_start(w->last + 1) == hi && w->last - w->first < MONTH_WINDOW_MAX;
}

static bool
read_cube(ScopeState *st, SiftF2Scope *sc, const SiftScopeL2Root *root, int64 key, SiftScopeQueryNeeds q, const MonthWindow *w, SiftScopeCubeAcc *out)
{
	SiftScopeDeltaStats stats = {0};
	bool		any = false;

	if (sc->nchanged > 0)
	{
		bool		exact;
		const bool	has = sift_f2_scope_key_cube(sc, key, &q, out, &exact);

		st->inexact |= !exact;
		return has;
	}
	if (root == NULL)
		return false;
	if (!w->ranged)
		return sift_scope_cube_live(&sc->meta, root, &q, &sc->entity, out, &stats);
	for (int32 m = w->first; m <= w->last; m++)
	{
		SiftScopeCubeAcc c;

		q.month = m;
		if (!sift_scope_cube_live(&sc->meta, root, &q, &sc->entity, any ? &c : out, &stats))
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
		q.month = months[i];
		if (!sift_scope_cube_live(&sc->meta, root, &q, &sc->entity, &c, &stats))
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

/* The members of the pairs of the key K, as the snapshot sees the relation of the pairs: the array the scan of the facts takes as the list of the member column */
static Datum *
push_member(Datum *vals, int n, int *cap, Datum m)
{
	if (n == *cap)
	{
		*cap *= 2;
		sift_budget_check((Size) *cap * sizeof(Datum), work_mem, "the members of a scope key", "work_mem", NULL);
		vals = repalloc(vals, (Size) *cap * sizeof(Datum));
	}
	vals[n] = m;
	return vals;
}

static ArrayType *
key_members(ScopeState *st, EState *estate)
{
	const ScopeSpec *sp = st->spec;
	Relation	pairs = table_open(sp->pairs_rel, AccessShareLock);
	const Oid	member_type = TupleDescAttr(RelationGetDescr(pairs), sp->pairs_member_att - 1)->atttypid;
	TupleTableSlot *slot = table_slot_create(pairs, NULL);
	TableScanDesc scan = sift_table_beginscan(pairs, estate->es_snapshot, 0, NULL);
	bool		no_key;
	const int64 key = key_value(st, &no_key);
	Datum	   *vals = palloc(sizeof(Datum));
	int			n = 0,
				cap = 1;

	while (!no_key && table_scan_getnextslot(scan, ForwardScanDirection, slot))
	{
		bool		key_null,
					member_null;
		const Datum k = slot_getattr(slot, sp->pairs_key_att, &key_null);
		const Datum m = slot_getattr(slot, sp->pairs_member_att, &member_null);

		if (key_null || member_null || int_of(sp->key_type, k) != key)
			continue;
		vals = push_member(vals, n++, &cap, m);
	}
	table_endscan(scan);
	ExecDropSingleTupleTableSlot(slot);
	table_close(pairs, AccessShareLock);
	st->members = n;
	return construct_array_builtin(vals, n, member_type);
}

/* The facts of the key K are the facts whose member is one of its members: the scan of the index with the list among its keys */
static void
scan_run(ScopeState *st, EState *estate)
{
	MemoryContext old = MemoryContextSwitchTo(estate->es_query_cxt);
	ScanKey		keys = palloc((st->nscan_keys + 1) * sizeof(ScanKeyData));

	st->started = true;
	memcpy(keys, st->scan_keys, st->nscan_keys * sizeof(ScanKeyData));
	keys[st->nscan_keys] = (ScanKeyData) {.sk_flags = SK_SEARCHARRAY,.sk_attno = st->spec->member_col,.sk_strategy = BTEqualStrategyNumber,
	.sk_argument = PointerGetDatum(key_members(st, estate))};
	st->gcx = sift_mem_allocset_create(estate->es_query_cxt, "sift agg v6 scope groups");
	st->group = sift_v6_group_new(st->v6, st->gcx, st->css.ss.ss_ScanTupleSlot->tts_tupleDescriptor);
	sift_v6_scan_run(st->v6, st->group, st->heap, st->rel, keys, st->nscan_keys + 1, estate->es_snapshot, NULL, 0, &st->stats, NULL, NULL);
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

/* The rows of the ring as the tuples of the scan, in the order the ring has them; false when a leaf holds no value of a row the heap has (the cell of an exception) */
static bool
topk_rows(ScopeState *st, SiftF2Scope *sc, const SiftScopeNewest *rows, int n)
{
	const int	nk = st->v6->nkeys;
	int			leaves[SIFT_V6_MAX_KEYS];
	SiftF2ScopeCell *cells = palloc(Max(n * nk, 1) * sizeof(SiftF2ScopeCell));

	for (int j = 0; j < nk; j++)
		leaves[j] = sift_col_rel(st->rel)->first[st->v6->key[j].attno];
	sift_f2_scope_cells(sc, rows, n, leaves, nk, cells);
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
	st->nrows = n;
	return true;
}

typedef struct TieRow
{
	SiftScopeNewest e;
	int64		tie;
} TieRow;

static int
tie_row_cmp(const void *a, const void *b, void *desc)
{
	const TieRow *x = a,
			   *y = b;

	if (x->e.time != y->e.time)
		return x->e.time > y->e.time ? -1 : 1;
	if (x->tie != y->tie)
		return (x->tie < y->tie) != *(bool *) desc ? -1 : 1;
	return 0;
}

/*
 * The first n rows of the ring leaf in the order of the second leaf where the ring leaf is equal. Where the cut falls inside the rows of one value the ring does not hold them all: the live rows
 * of the key with that value are read whole (sift_scope_ring_ties). Returns the rows kept (n), or -1 when the leaf holds no value of a row.
 */
static int
topk_tie_order(ScopeState *st, SiftF2Scope *sc, const SiftScopeL2Root *root, int ring, SiftScopeNewest *rows, int n, bool tie_at_cut)
{
	const int	leaf = sift_col_rel(st->rel)->first[st->spec->tie_col];
	SiftScopeNewest *flat = rows;
	SiftF2ScopeCell *cells;
	TieRow	   *c;
	int			total = n;

	if (tie_at_cut)
	{
		SiftScopeNewest *ties;
		int			keep = 0;
		const int	nties = (int) sift_scope_ring_ties(&sc->meta, root, ring, rows[n - 1].time, &sc->entity, &ties);

		while (rows[keep].time != rows[n - 1].time)
			keep++;
		total = keep + nties;
		if (total < n)
			return -1;
		flat = palloc(total * sizeof(SiftScopeNewest));
		memcpy(flat, rows, keep * sizeof(SiftScopeNewest));
		memcpy(flat + keep, ties, nties * sizeof(SiftScopeNewest));
	}
	cells = palloc(total * sizeof(SiftF2ScopeCell));
	sift_f2_scope_cells(sc, flat, total, &leaf, 1, cells);
	c = palloc(total * sizeof(TieRow));
	for (int i = 0; i < total; i++)
	{
		if (cells[i].isnull)
			return -1;
		c[i] = (TieRow) {flat[i], cells[i].i};
	}
	qsort_arg(c, total, sizeof(TieRow), tie_row_cmp, (void *) &st->spec->tie_desc);
	for (int i = 0; i < n; i++)
		rows[i] = c[i].e;
	return n;
}

/*
 * ORDER BY ring leaf DESC [, tie leaf] LIMIT n of the facts of the key: the first n entries of its ring (of the rows with one value, the order among them is the ring's, or the tie leaf's). The ring is
 * NULLS LAST, so it is the order of the statement when the leaf cannot be NULL or the statement puts the NULLs last and the key has n rows with a value. Under edits the ring's rows that the snapshot
 * does not see are skipped (the entity tells), and it answers only when none of the rows it lacks can rank among the first n.
 */
static bool
topk_read(ScopeState *st, SiftF2Scope *sc)
{
	const ScopeSpec *sp = st->spec;
	bool		isnull,
				tie_at_cut;
	const int64 key = key_value(st, &isnull);
	const SiftScopeL2Root *root = isnull ? NULL : sift_scope_l2_find(&sc->meta, key);
	const int	ring = sift_f2_scope_ring_of(sc, sp->ring_col);
	SiftScopeNewest *rows = palloc(sp->limit * sizeof(SiftScopeNewest));
	uint32		from_ring;
	int			n;

	if (!sift_f2_scope_rings(sc))
		SIFT_CORRUPT(sc->rel, "The summary of scope %d is damaged.", sp->ord);
	if (ring < 0 || !sift_scope_ring_serves(&sc->meta, ring, sp->nulls_first, sp->leaf_not_null))
		return false;
	if (root == NULL)
		return !sift_f2_scope_added_rank(sc, key, ring, sp->limit, NULL);
	n = (int) sift_scope_latest_ring(&sc->meta, root, ring, sp->limit, false, &sc->entity, rows, &from_ring, sp->tie_col > 0 ? &tie_at_cut : NULL);
	if ((n != sp->limit && !sp->leaf_not_null) || sift_f2_scope_added_rank(sc, key, ring, sp->limit - n, n > 0 ? &rows[n - 1] : NULL))
		return false;
	if (sp->tie_col > 0 && n > 0 && (n = topk_tie_order(st, sc, root, ring, rows, n, tie_at_cut)) < 0)
		return false;
	return topk_rows(st, sc, rows, n);
}

/* The summary for the statement's snapshot: of the keys it names the pairs may have been edited since; a read of every key, of months or of a window or ring answers only from pairs as they were */
static SiftF2Scope *
open_summary(ScopeState *st, EState *estate, const int64 *keys, uint32 nkeys)
{
	const ScopeSpec *sp = st->spec;
	SiftF2Scope *sc = sift_f2_scope_open_keys(st->rel, sp->ord, estate->es_snapshot, sp->excl_col, keys, nkeys);

	if (sc != NULL && sc->nchanged > 0 && (sp->by_month || sp->ranged || sp->topk))
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
			st->nrows = st->at = 0;
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
	st->stats = (SiftV6ScanStats) {0};
}

static void
scope_reset(ScopeState *st)
{
	scan_reset(st);
	MemoryContextReset(st->rcx);
	st->vals = NULL;
	st->nulls = NULL;
	st->nrows = st->at = 0;
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
