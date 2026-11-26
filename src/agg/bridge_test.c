/*
 * agg/bridge_test.c - test-only SQL functions sift._test_v6_*, compiled only with SIFT_TESTING and registered by the pg_sift_test extension: the guards of agg/safety.h and the
 * bridges of SiftAggV6 (v6_topn.h, v6_window.h, v6_stream.h, v6_sip.h, v6_card.h, v6_vexpr.h) held to PostgreSQL's own answer.
 *
 * The format the bridges run over is a table: an index whose name ends in _v6 is one the test format owns, its columns are the columns of the table it names, the parts are the rows
 * sorted by them (ascending, NULLs last), and a source reads the table with the snapshot of the statement into batches of TEST_BATCH rows. The bridges are put in the planner by the hooks
 * of sift._test_v6_check, which runs a query with them and without and compares the plan and the rows.
 *
 * check-memory: allow-allocs the arrays follow the rows of a test table, a few thousand
 */
#include "postgres.h"

#ifdef SIFT_TESTING

#include "access/genam.h"
#include "access/heapam.h"
#include "access/htup_details.h"
#include "access/table.h"
#include "access/tableam.h"
#include "catalog/pg_type_d.h"
#include "executor/spi.h"
#include "fmgr.h"
#include "funcapi.h"
#include "miscadmin.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "optimizer/optimizer.h"
#include "optimizer/paths.h"
#include "optimizer/planner.h"
#include "parser/analyze.h"
#include "rewrite/rewriteHandler.h"
#include "tcop/tcopprot.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"
#include "utils/numeric.h"
#include "utils/rel.h"

#include "agg/safety.h"
#include "agg/cost/card.h"
#include "agg/sip_scan.h"
#include "agg/stream.h"
#include "agg/topn/topn.h"
#include "agg/win/window.h"
#include "core/catname.h"
#include "core/memtrack.h"
#include "core/compat.h"

PG_FUNCTION_INFO_V1(sift_test_agg_guards);
PG_FUNCTION_INFO_V1(sift_test_agg_query_rls);
PG_FUNCTION_INFO_V1(sift_test_v6_filter);
PG_FUNCTION_INFO_V1(sift_test_v6_check);
PG_FUNCTION_INFO_V1(sift_test_v6_card);

#define TEST_BATCH		700
#define TEST_CHUNK		256

/* ---- the guards ---- */

static int
expect(bool got, bool want)
{
	return got != want;
}

Datum
sift_test_agg_guards(PG_FUNCTION_ARGS)
{
	Param	   *ext = makeNode(Param);
	Param	   *exec = makeNode(Param);
	OpExpr	   *op = makeNode(OpExpr);
	IndexOptInfo ix = {0};
	int			wrong = 0;

	ext->paramkind = PARAM_EXTERN;
	exec->paramkind = PARAM_EXEC;
	op->args = list_make2(makeConst(INT4OID, -1, InvalidOid, 4, Int32GetDatum(1), false, true), ext);
	wrong += expect(agg_node_has_extern_param((Node *) op), true);
	wrong += expect(agg_node_has_extern_param((Node *) exec), false);
	wrong += expect(agg_node_has_extern_param((Node *) list_make1(makeConst(INT4OID, -1, InvalidOid, 4, Int32GetDatum(1), false, true))), false);
	wrong += expect(agg_node_has_extern_param(NULL), false);

	wrong += expect(agg_eflags_rescannable(0), false);
	wrong += expect(agg_eflags_rescannable(EXEC_FLAG_EXPLAIN_ONLY), false);
	wrong += expect(agg_eflags_rescannable(EXEC_FLAG_REWIND), true);
	wrong += expect(agg_eflags_rescannable(EXEC_FLAG_BACKWARD), true);
	wrong += expect(agg_eflags_rescannable(EXEC_FLAG_MARK | EXEC_FLAG_SKIP_TRIGGERS), true);

	wrong += expect(agg_index_complete(&ix), true);
	ix.hypothetical = true;
	wrong += expect(agg_index_complete(&ix), false);
	ix.hypothetical = false;
	ix.indpred = list_make1(makeBoolConst(true, false));
	wrong += expect(agg_index_complete(&ix), false);
	ix.predOK = true;
	wrong += expect(agg_index_complete(&ix), true);

	ix.ncolumns = 3;
	ix.indexkeys = (int[]) {2, 0, 5};
	wrong += expect(agg_index_col(&ix, 2) == 0, true);
	wrong += expect(agg_index_col(&ix, 5) == 2, true);
	wrong += expect(agg_index_col(&ix, 3) == -1, true);
	wrong += expect(agg_index_col(&ix, 0) == -1, true);
	PG_RETURN_INT32(wrong);
}

static Query *
rewritten(const char *sql)
{
	RawStmt    *raw = linitial(pg_parse_query(sql));
	List	   *qs = pg_analyze_and_rewrite_fixedparams(raw, sql, NULL, 0, NULL);

	return linitial_node(Query, qs);
}

Datum
sift_test_agg_query_rls(PG_FUNCTION_ARGS)
{
	PlannerInfo *root = makeNode(PlannerInfo);
	char	   *sql = text_to_cstring(PG_GETARG_TEXT_PP(0));

	root->parse = rewritten(sql);
	PG_RETURN_BOOL(agg_query_has_rls(root));
}

/* ---- the table the test format reads ---- */

typedef struct TestTable
{
	MemoryContext cx;
	TupleDesc	desc;
	int			nkeys;
	AttrNumber	keys[INDEX_MAX_KEYS];
	int			nrows;
	HeapTuple  *rows;
} TestTable;

static int64
numeric_scaled(Datum d, int scale)
{
	bool		bad = false;
	int64		pow10 = 1;

	for (int i = 0; i < scale; i++)
		pow10 *= 10;
	return sift_numeric_int8(sift_numeric_mul(DatumGetNumeric(d), int64_to_numeric(pow10), &bad), &bad);
}

static int64
value_i64(Oid typid, int scale, Datum d)
{
	switch (typid)
	{
		case INT2OID:
			return DatumGetInt16(d);
		case INT4OID:
		case DATEOID:
			return DatumGetInt32(d);
		case BOOLOID:
			return DatumGetBool(d);
		case NUMERICOID:
			return numeric_scaled(d, scale);
		case FLOAT8OID:
			return sift_f64_order_i64(DatumGetFloat8(d));
		default:
			return DatumGetInt64(d);
	}
}

static int
numeric_scale_of(const TupleDesc desc, AttrNumber att)
{
	SiftV6InCol c;

	return sift_v6_vec_type(NUMERICOID, TupleDescAttr(desc, att - 1)->atttypmod, &c) ? c.scale : 0;
}

static int
cmp_rows(const void *pa, const void *pb, void *arg)
{
	const TestTable *t = arg;
	HeapTuple	a = *(HeapTuple *) pa,
				b = *(HeapTuple *) pb;

	for (int k = 0; k < t->nkeys; k++)
	{
		const Form_pg_attribute at = TupleDescAttr(t->desc, t->keys[k] - 1);
		bool		na,
					nb;
		const Datum da = heap_getattr(a, t->keys[k], t->desc, &na),
					db = heap_getattr(b, t->keys[k], t->desc, &nb);
		int64		x,
					y;

		if (na || nb)
		{
			if (na != nb)
				return na ? 1 : -1;
			continue;
		}
		x = value_i64(at->atttypid, numeric_scale_of(t->desc, t->keys[k]), da);
		y = value_i64(at->atttypid, numeric_scale_of(t->desc, t->keys[k]), db);
		if (x != y)
			return x < y ? -1 : 1;
	}
	return 0;
}

static TestTable *
table_load(MemoryContext cx, EState *estate, Oid indexoid, bool sorted)
{
	TestTable  *t = MemoryContextAllocZero(cx, sizeof(TestTable));
	Relation	index = index_open(indexoid, AccessShareLock);
	Relation	heap = table_open(index->rd_index->indrelid, AccessShareLock);
	MemoryContext old = MemoryContextSwitchTo(cx);
	TableScanDesc scan = sift_table_beginscan(heap, estate->es_snapshot, 0, NULL);
	TupleTableSlot *slot = table_slot_create(heap, NULL);
	int			cap = 1024;

	t->cx = cx;
	t->nkeys = index->rd_index->indnatts;
	for (int i = 0; i < t->nkeys; i++)
		t->keys[i] = index->rd_index->indkey.values[i];
	t->desc = CreateTupleDescCopy(RelationGetDescr(heap));
	t->rows = palloc(cap * sizeof(HeapTuple));
	while (table_scan_getnextslot(scan, ForwardScanDirection, slot))
	{
		if (t->nrows == cap)
			t->rows = repalloc(t->rows, (cap *= 2) * sizeof(HeapTuple));
		t->rows[t->nrows++] = ExecCopySlotHeapTuple(slot);
	}
	ExecDropSingleTupleTableSlot(slot);
	table_endscan(scan);
	MemoryContextSwitchTo(old);
	table_close(heap, AccessShareLock);
	index_close(index, AccessShareLock);
	if (sorted)
		qsort_arg(t->rows, t->nrows, sizeof(HeapTuple), cmp_rows, t);
	return t;
}

/* ---- the batches of a table ---- */

typedef struct TestCore
{
	MemoryContext cx;
	TestTable  *t;
	SiftV6Input in;
	SiftV6Vexpr quals;
	bool		has_quals;
	SiftVexprCtx *vctx;
	SiftVec		vec[SIFT_V6_MAX_COLS];
	void	   *data[SIFT_V6_MAX_COLS];
	uint64	   *valid[SIFT_V6_MAX_COLS];
	SiftVexprMaskIn masks[SIFT_V6_MAX_MASKS];
	uint64	   *mbits[SIFT_V6_MAX_MASKS];
	uint64	   *mvalid[SIFT_V6_MAX_MASKS];
	uint64		mask[SIFT_VEC_WORDS];
	uint64		tag[SIFT_VEC_LEN];
} TestCore;

static size_t
elem_size(SiftVecType t)
{
	return t == SVEC_I32 ? sizeof(int32) : sizeof(int64);
}

static void
core_zone(TestCore *c)
{
	int64		lo = INT64_MAX,
				hi = INT64_MIN;

	for (int i = 0; i < c->in.ncols; i++)
		if (c->in.col[i].typid == TIMESTAMPTZOID || c->in.col[i].typid == TIMESTAMPOID)
			for (int r = 0; r < c->t->nrows; r++)
			{
				bool		isnull;
				const Datum d = heap_getattr(c->t->rows[r], c->t->keys[c->in.col[i].attno - 1], c->t->desc, &isnull);

				if (!isnull)
				{
					lo = Min(lo, DatumGetInt64(d));
					hi = Max(hi, DatumGetInt64(d));
				}
			}
	if (c->quals.zone != NULL && lo <= hi)
		zone_cover(c->quals.zone, lo, hi);
}

static TestCore *
core_new(EState *estate, Oid indexoid, bool sorted, const SiftV6Input *in, const SiftV6Vexpr *quals)
{
	MemoryContext cx = sift_mem_allocset_create(estate->es_query_cxt, "sift v6 test source");
	TestCore   *c = MemoryContextAllocZero(cx, sizeof(TestCore));
	MemoryContext old = MemoryContextSwitchTo(cx);

	c->cx = cx;
	c->in = *in;
	c->t = table_load(cx, estate, indexoid, sorted);
	if (quals != NULL)
	{
		c->quals = *quals;
		c->has_quals = true;
	}
	c->vctx = sift_vexpr_ctx_new(cx);
	for (int i = 0; i < in->ncols; i++)
	{
		c->data[i] = palloc_aligned((SIFT_VEC_LEN + SIFT_VEC_PAD) * elem_size(in->col[i].type), SIFT_VEC_ALIGN, MCXT_ALLOC_ZERO);
		c->valid[i] = palloc_aligned(SIFT_VEC_WORDS * sizeof(uint64), SIFT_VEC_ALIGN, MCXT_ALLOC_ZERO);
	}
	for (int m = 0; m < in->nmasks; m++)
	{
		c->mbits[m] = palloc0(SIFT_VEC_WORDS * sizeof(uint64));
		c->mvalid[m] = palloc0(SIFT_VEC_WORDS * sizeof(uint64));
		c->masks[m] = (SiftVexprMaskIn) {.bits = c->mbits[m],.valid = c->mvalid[m]};
	}
	MemoryContextSwitchTo(old);
	core_zone(c);
	return c;
}

static void
fill_col(TestCore *c, int i, int from, int n)
{
	const SiftV6InCol *ic = &c->in.col[i];
	const AttrNumber att = c->t->keys[ic->attno - 1];

	memset(c->valid[i], 0, SIFT_VEC_WORDS * sizeof(uint64));
	for (int r = 0; r < n; r++)
	{
		bool		isnull;
		const Datum d = heap_getattr(c->t->rows[from + r], att, c->t->desc, &isnull);

		if (isnull)
		{
			if (ic->type == SVEC_I32)
				((int32 *) c->data[i])[r] = 0;
			else
				((int64 *) c->data[i])[r] = 0;
			continue;
		}
		c->valid[i][r / 64] |= UINT64_C(1) << (r % 64);
		if (ic->type == SVEC_I32)
			((int32 *) c->data[i])[r] = (int32) value_i64(ic->typid, ic->scale, d);
		else if (ic->type == SVEC_F64)
			((double *) c->data[i])[r] = DatumGetFloat8(d);
		else
			((int64 *) c->data[i])[r] = value_i64(ic->typid, ic->scale, d);
	}
	c->vec[i] = (SiftVec) {.data = c->data[i],.valid = c->valid[i],.len = (uint16) n,.all_valid = false,.dense = true,.type = ic->type,.scale = ic->scale};
}

static void
fill_mask(TestCore *c, int m, int from, int n)
{
	const AttrNumber att = c->t->keys[c->in.mask[m].attno - 1];

	memset(c->mbits[m], 0, SIFT_VEC_WORDS * sizeof(uint64));
	memset(c->mvalid[m], 0, SIFT_VEC_WORDS * sizeof(uint64));
	for (int r = 0; r < n; r++)
	{
		bool		isnull;
		const Datum d = heap_getattr(c->t->rows[from + r], att, c->t->desc, &isnull);

		if (isnull)
			continue;
		c->mvalid[m][r / 64] |= UINT64_C(1) << (r % 64);
		if (DatumGetBool(d))
			c->mbits[m][r / 64] |= UINT64_C(1) << (r % 64);
	}
}

/* the rows from .. from + n of the table as a batch; false when its quals leave none */
static bool
core_batch(TestCore *c, int from, int n, SiftV6Batch *b)
{
	for (int i = 0; i < c->in.ncols; i++)
		fill_col(c, i, from, n);
	for (int m = 0; m < c->in.nmasks; m++)
		fill_mask(c, m, from, n);
	for (int r = 0; r < n; r++)
		c->tag[r] = (uint64) (from + r);
	*b = (SiftV6Batch) {.n = (uint16) n,.col = c->vec,.masks = c->masks,.tag = c->tag};
	if (c->has_quals)
	{
		if (sift_v6_vexpr_filter(c->vctx, &c->quals, b, c->mask) == 0)
			return false;
		b->mask = c->mask;
	}
	return true;
}

static bool
core_fetch(TestCore *c, uint64 tag, const AttrNumber *attnos, int n, Datum *values, bool *isnull)
{
	if (tag >= (uint64) c->t->nrows)
		return false;
	for (int i = 0; i < n; i++)
		values[i] = heap_getattr(c->t->rows[tag], attnos[i], c->t->desc, &isnull[i]);
	return true;
}

typedef struct TestSource
{
	SiftV6Source base;
	TestCore   *c;
	int			pos;
} TestSource;

static bool
src_next(SiftV6Source *s, SiftV6Batch *out)
{
	TestSource *ts = (TestSource *) s;

	while (ts->pos < ts->c->t->nrows)
	{
		const int	from = ts->pos,
					n = Min(TEST_BATCH, ts->c->t->nrows - from);

		ts->pos += n;
		if (core_batch(ts->c, from, n, out))
			return true;
	}
	return false;
}

static bool
src_fetch(SiftV6Source *s, uint64 tag, const AttrNumber *attnos, int n, Datum *values, bool *isnull)
{
	return core_fetch(((TestSource *) s)->c, tag, attnos, n, values, isnull);
}

static void
src_close(SiftV6Source *s)
{
	MemoryContextDelete(((TestSource *) s)->c->cx);
}

/* ---- the test format ---- */

static bool
fmt_owns_rel(Oid relid, bool is_table)
{
	const char *name = get_rel_name(relid);
	const size_t len = name == NULL ? 0 : strlen(name);

	return !is_table && len > 3 && strcmp(name + len - 3, "_v6") == 0;
}

static bool
fmt_leaf_holds(Oid indexoid, bool is_table, int att, int cls)
{
	return cls != SV6C_NONE && sift_v6_class(get_atttype(indexoid, att)) == cls;
}

static bool
fmt_sorted_by(Oid indexoid, bool is_table, const int *atts, int n)
{
	for (int i = 0; i < n; i++)
		if (atts[i] != i + 1)
			return false;
	return true;
}

static int
fmt_card_leaf(Oid indexoid, int col)
{
	return col;
}

static SiftV6Source *
fmt_open(EState *estate, const SiftV6Input *in, const SiftV6Vexpr *quals, bool sorted)
{
	TestSource *ts = palloc0(sizeof(TestSource));

	ts->c = core_new(estate, in->indexoid, sorted, in, quals);
	ts->base = (SiftV6Source) {.next = src_next,.fetch = src_fetch,.close = src_close};
	return &ts->base;
}

/* the test format keeps no leaf map, so the aggregate scan plans nothing over it: the operators read it through its source, its order and its card leaves */
static bool
fmt_schema(Oid relid, bool is_table, SiftStoreSchema *out)
{
	return false;
}

static bool
fmt_stats(Oid relid, bool is_table, SiftStoreStats *out)
{
	*out = (SiftStoreStats) {.card_leaf = fmt_card_leaf};
	return true;
}

static bool topn_open(EState *estate, const SiftV6TopNSpec *spec, const SiftV6Input *in, const SiftV6Vexpr *quals, SiftV6TopNSource *dst);

static const SiftOrderOps test_order = {.sorted_by = fmt_sorted_by,.topn_open = topn_open};

static const SiftStoreOps test_ops = {
	.name = "test",
	.any_am = true,
	.owns_rel = fmt_owns_rel,
	.schema = fmt_schema,
	.stats = fmt_stats,
	.open = fmt_open,
	.leaf_holds = fmt_leaf_holds,
	.order = &test_order
};

/* ---- the Top-N source: chunks of the table as the units of the tournament ---- */

typedef struct TestTopN
{
	TestCore   *c;
	const SiftV6TopNSpec *spec;
} TestTopN;

static int64
key_of(const TestTopN *tn, HeapTuple row, int j, bool *isnull)
{
	const SiftV6Col *k = &tn->spec->key[j];
	const AttrNumber att = tn->c->t->keys[k->attno - 1];
	const Datum d = heap_getattr(row, att, tn->c->t->desc, isnull);

	return *isnull ? 0 : value_i64(k->typid, k->scale, d);
}

static SiftTournBound
chunk_bound(const TestTopN *tn, int j, int from, int to)
{
	SiftTournBound b = {.known = true,.all_null = true,.min = INT64_MAX,.max = INT64_MIN};

	for (int r = from; r < to; r++)
	{
		bool		isnull;
		const int64 v = key_of(tn, tn->c->t->rows[r], j, &isnull);

		if (isnull)
			b.has_null = true;
		else
		{
			b.all_null = false;
			b.min = Min(b.min, v);
			b.max = Max(b.max, v);
		}
	}
	return b;
}

static void
topn_seed(void *src, SiftTournament *t)
{
	const TestTopN *tn = src;

	for (int g = 0; g * TEST_CHUNK < tn->c->t->nrows; g++)
	{
		const int	from = g * TEST_CHUNK,
					to = Min(from + TEST_CHUNK, tn->c->t->nrows);
		const SiftTournBound b0 = chunk_bound(tn, 0, from, to);
		const SiftTournBound b1 = tn->spec->nkeys > 1 ? chunk_bound(tn, 1, from, to) : (SiftTournBound) {0};

		sift_tourn_push(t, STOK_GRANULE, 0, (uint32) g, 0, &b0, tn->spec->nkeys > 1 ? &b1 : NULL);
	}
}

static void
topn_scan(void *src, SiftTournament *t, const SiftTopNSchedTok *tok)
{
	TestTopN   *tn = src;
	const int	from = (int) tok->local_g * TEST_CHUNK,
				n = Min(TEST_CHUNK, tn->c->t->nrows - from);
	SiftV6Batch b;

	if (!core_batch(tn->c, from, n, &b))
		return;
	for (int r = 0; r < n; r++)
	{
		SiftTopNWinRow row = {.tid = (uint64) (from + r),.local_g = (uint16) tok->local_g,.row_ord = (uint16) r};

		if (b.mask != NULL && !((b.mask[r / 64] >> (r % 64)) & 1))
			continue;
		for (int j = 0; j < tn->spec->nkeys; j++)
		{
			bool		isnull;

			row.key[j] = key_of(tn, tn->c->t->rows[from + r], j, &isnull);
			row.nulls |= (uint8) (isnull << j);
		}
		sift_tourn_offer(t, &row);
	}
}

static void
topn_unused(void *src, SiftTournament *t, const SiftTopNSchedTok *tok)
{
	elog(ERROR, "the test Top-N source has granules only");
}

static bool
topn_fetch(void *src, const SiftTopNWinRow *w, const AttrNumber *attnos, int n, Datum *values, bool *isnull)
{
	return core_fetch(((TestTopN *) src)->c, w->tid, attnos, n, values, isnull);
}

static void
topn_close(void *src)
{
	MemoryContextDelete(((TestTopN *) src)->c->cx);
}

static bool
topn_open(EState *estate, const SiftV6TopNSpec *spec, const SiftV6Input *in, const SiftV6Vexpr *quals, SiftV6TopNSource *dst)
{
	TestTopN   *tn = MemoryContextAllocZero(estate->es_query_cxt, sizeof(TestTopN));

	tn->c = core_new(estate, spec->indexoid, false, in, quals);
	tn->spec = spec;
	*dst = (SiftV6TopNSource) {.ops = {.expand = topn_unused,.frontier_next = topn_unused,.scan = topn_scan},.src = tn,.seed = topn_seed,.fetch = topn_fetch,.close = topn_close};
	return true;
}

/* ---- the planner side of the tests ---- */

typedef enum Bridge
{
	BR_TOPN = 1,
	BR_WINDOW = 2,
	BR_STREAM = 4,
	BR_SIP = 8,
	BR_CARD = 16
} Bridge;

static int	enabled;
static create_upper_paths_hook_type prev_upper_hook;
static set_rel_pathlist_hook_type prev_rel_hook;

typedef struct CardResult
{
	int			npreds;
	int			nrest;
	bool		replaced;
	double		pg_rows;
	double		rows;
	int			calls;
} CardResult;

static CardResult card_result;
static SiftColCardStats card_cols[8];
static SiftCardStatsView card_view;

static void
card_view_init(void)
{
	const struct
	{
		double		ndv;
		int64		max;
	}			shape[] = {{7, 6}, {100, 99}, {5000, 4999}, {50, 49}, {3000, 3000}, {200, 199}, {1000, 999}, {2, 1}};

	for (int i = 0; i < 8; i++)
	{
		SiftColCardStats *cs = &card_cols[i];

		*cs = (SiftColCardStats) {.col = (uint16) i,.flags = SIFT_CSTAT_HAS_RANGE | SIFT_CSTAT_HAS_NDV | SIFT_CSTAT_HAS_HIST,.n_hist = SIFT_CARD_HIST_BUCKETS,
			.total_rows = 100000,.ndv_est = shape[i].ndv,.min_i64 = 0,.max_i64 = shape[i].max};
		for (int b = 0; b <= SIFT_CARD_HIST_BUCKETS; b++)
			cs->hist_bounds[b] = shape[i].max * b / SIFT_CARD_HIST_BUCKETS;
	}
	card_view = (SiftCardStatsView) {.total_live_rows = 100000,.total_granules = 100,.ncols = 8,.cols = card_cols};
}

static void
test_upper_hook(PlannerInfo *root, UpperRelationKind stage, RelOptInfo *input_rel, RelOptInfo *output_rel, void *extra)
{
	if (prev_upper_hook)
		prev_upper_hook(root, stage, input_rel, output_rel, extra);
	if (enabled & BR_TOPN)
		sift_agg_v6_topn_upper_paths(root, stage, input_rel, output_rel, extra);
	if (enabled & BR_WINDOW)
		sift_agg_v6_window_upper_paths(root, stage, input_rel, output_rel, extra);
	if (enabled & BR_STREAM)
		sift_agg_v6_stream_upper_paths(root, stage, input_rel, output_rel, extra);
}

static void
test_rel_hook(PlannerInfo *root, RelOptInfo *rel, Index rti, RangeTblEntry *rte)
{
	if (prev_rel_hook)
		prev_rel_hook(root, rel, rti, rte);
	if (enabled & BR_SIP)
		sift_agg_v6_sip_rel_paths(root, rel, rti, rte);
	if ((enabled & BR_CARD) && rel->reloptkind == RELOPT_BASEREL && rte->rtekind == RTE_RELATION)
		foreach_ptr(IndexOptInfo, ix, rel->indexlist)
		{
			SiftV6CardQuery cq;

			if (!fmt_owns_rel(ix->indexoid, false))
				continue;
			sift_v6_card_query(rel->baserestrictinfo, rti, ix, &cq);
			card_result = (CardResult) {.npreds = cq.q.npreds,.nrest = list_length(cq.rest),.pg_rows = rel->rows,.calls = card_result.calls + 1};
			card_result.replaced = sift_v6_card_rel(root, rel, ix, &card_view);
			card_result.rows = rel->rows;
			break;
		}
}

static void
register_once(void)
{
	static bool done;

	if (done)
		return;
	sift_store_ops_register(&test_ops);
	card_view_init();
	done = true;
}

static int
bridges_of(ArrayType *names)
{
	Datum	   *e;
	bool	   *nulls;
	int			n,
				mask = 0;

	if (names == NULL)
		return 0;
	deconstruct_array(names, TEXTOID, -1, false, TYPALIGN_INT, &e, &nulls, &n);
	for (int i = 0; i < n; i++)
	{
		const char *s = TextDatumGetCString(e[i]);

		mask |= strcmp(s, "topn") == 0 ? BR_TOPN : strcmp(s, "window") == 0 ? BR_WINDOW : strcmp(s, "stream") == 0 ? BR_STREAM : strcmp(s, "sip") == 0 ? BR_SIP :
			strcmp(s, "card") == 0 ? BR_CARD : 0;
	}
	return mask;
}

/* the lines of a statement's result, a row as its columns joined by | (NULL as NULL), run with the bridges on */
static List *
run_lines(const char *sql, int bridges)
{
	List	   *lines = NIL;
	const int	was = enabled;
	create_upper_paths_hook_type up = create_upper_paths_hook;
	set_rel_pathlist_hook_type rl = set_rel_pathlist_hook;
	MemoryContext cx = CurrentMemoryContext;

	enabled = bridges;
	if (bridges == 0)
		sift_store_ops_unregister(&test_ops);
	prev_upper_hook = up;
	prev_rel_hook = rl;
	create_upper_paths_hook = test_upper_hook;
	set_rel_pathlist_hook = test_rel_hook;
	PG_TRY();
	{
		if (SPI_execute(sql, false, 0) < 0)
			elog(ERROR, "sift._test_v6: the statement failed");
		for (uint64 r = 0; r < SPI_processed; r++)
		{
			StringInfoData line;
			MemoryContext old = MemoryContextSwitchTo(cx);

			initStringInfo(&line);
			for (int c = 1; c <= SPI_tuptable->tupdesc->natts; c++)
			{
				const char *v = SPI_getvalue(SPI_tuptable->vals[r], SPI_tuptable->tupdesc, c);

				appendStringInfo(&line, c > 1 ? "|%s" : "%s", v == NULL ? "NULL" : v);
			}
			lines = lappend(lines, line.data);
			MemoryContextSwitchTo(old);
		}
	}
	PG_FINALLY();
	{
		create_upper_paths_hook = up;
		set_rel_pathlist_hook = rl;
		sift_store_ops_register(&test_ops);
		enabled = was;
	}
	PG_END_TRY();
	return lines;
}

static int
cmp_cstr(const ListCell *a, const ListCell *b)
{
	return strcmp(lfirst(a), lfirst(b));
}

/* sift._test_v6_check(query, node, bridges, ordered): 'ok', or what differs between the plan and the rows with the bridges and without */
Datum
sift_test_v6_check(PG_FUNCTION_ARGS)
{
	const char *sql = text_to_cstring(PG_GETARG_TEXT_PP(0));
	const char *node = text_to_cstring(PG_GETARG_TEXT_PP(1));
	const int	bridges = bridges_of(PG_ARGISNULL(2) ? NULL : PG_GETARG_ARRAYTYPE_P(2));
	const bool	ordered = PG_GETARG_BOOL(3);
	const char *verdict = "ok";
	List	   *plan,
			   *with,
			   *without;
	bool		found = false;

	SPI_connect();
	register_once();
	plan = run_lines(psprintf("EXPLAIN (COSTS OFF) %s", sql), bridges);
	foreach_ptr(char, l, plan)
		found |= strstr(l, node[0] == '\0' ? "SiftAggV6" : node) != NULL;
	with = run_lines(sql, bridges);
	without = run_lines(sql, 0);
	if (node[0] != '\0' && !found)
		verdict = "the plan has no such node";
	else if (node[0] == '\0' && found)
		verdict = "the plan has a node of the bridges";
	else if (list_length(with) != list_length(without))
		verdict = psprintf("%d rows with the bridges, %d without", list_length(with), list_length(without));
	else
	{
		if (!ordered)
		{
			list_sort(with, cmp_cstr);
			list_sort(without, cmp_cstr);
		}
		for (int i = 0; i < list_length(with); i++)
			if (strcmp(list_nth(with, i), list_nth(without, i)) != 0)
			{
				verdict = psprintf("row %d: %s, without the bridges %s", i + 1, (char *) list_nth(with, i), (char *) list_nth(without, i));
				break;
			}
	}
	SPI_finish();
	PG_RETURN_TEXT_P(cstring_to_text(verdict));
}

/*
 * sift._test_v6_filter(index, where, param): the ids of the rows of the table of the index (its column id) that pass the WHERE clause (over its columns, $1 an int8) as the machine
 * evaluates it, in the order of the table; NULL when the machine does not run it. The program is made twice as the planner and the executor do: with no parameters, then with this one.
 */
Datum
sift_test_v6_filter(PG_FUNCTION_ARGS)
{
	const Oid	indexoid = PG_GETARG_OID(0);
	const char *where = text_to_cstring(PG_GETARG_TEXT_PP(1));
	Relation	index = index_open(sift_rel_must_exist(indexoid), AccessShareLock);
	const Oid	heapoid = index->rd_index->indrelid;
	Oid			argtypes[1] = {INT8OID};
	RawStmt    *raw;
	Query	   *q;
	List	   *quals;
	SiftV6Input plan_in,
				run_in;
	SiftV6Vexpr plan_prog,
				run_prog;
	ParamListInfo params = makeParamList(1);
	EState	   *estate = CreateExecutorState();
	TestCore   *c;
	List	   *ids = NIL;
	const AttrNumber idatt = get_attnum(heapoid, "id");

	register_once();
	raw = linitial(pg_parse_query(psprintf("SELECT 1 FROM %s WHERE %s", quote_qualified_identifier(NULL, get_rel_name(heapoid)), where)));
	q = linitial_node(Query, pg_analyze_and_rewrite_fixedparams(raw, "", argtypes, 1, NULL));
	quals = make_ands_implicit((Expr *) eval_const_expressions(NULL, q->jointree->quals));
	params->params[0] = (ParamExternData) {.value = PG_ARGISNULL(2) ? (Datum) 0 : PG_GETARG_DATUM(2),.isnull = PG_ARGISNULL(2),.pflags = PARAM_FLAG_CONST,.ptype = INT8OID};
	sift_v6_input_init_rel(&plan_in, index);
	sift_v6_input_init_rel(&run_in, index);
	index_close(index, AccessShareLock);
	if (!sift_v6_vexpr_quals(CurrentMemoryContext, quals, 1, NULL, &plan_in, &plan_prog) ||
		!sift_v6_vexpr_quals(CurrentMemoryContext, quals, 1, params, &run_in, &run_prog))
		PG_RETURN_NULL();
	estate->es_snapshot = GetActiveSnapshot();
	c = core_new(estate, indexoid, false, &run_in, &run_prog);
	for (int from = 0; from < c->t->nrows; from += TEST_BATCH)
	{
		const int	n = Min(TEST_BATCH, c->t->nrows - from);
		SiftV6Batch b;

		if (!core_batch(c, from, n, &b))
			continue;
		for (int r = 0; r < n; r++)
			if ((b.mask[r / 64] >> (r % 64)) & 1)
			{
				bool		isnull;

				ids = lappend(ids, (void *) heap_getattr(c->t->rows[from + r], idatt, c->t->desc, &isnull));
			}
	}
	{
		Datum	   *vals = palloc(Max(list_length(ids), 1) * sizeof(Datum));
		int			i = 0;

		foreach_ptr(void, id, ids)
			vals[i++] = (Datum) id;
		FreeExecutorState(estate);
		PG_RETURN_ARRAYTYPE_P(construct_array_builtin(vals, i, INT8OID));
	}
}

/* sift._test_v6_card(query): what the clauses of the table the query reads become, as the planner hook of the card bridge saw them */
Datum
sift_test_v6_card(PG_FUNCTION_ARGS)
{
	const char *sql = text_to_cstring(PG_GETARG_TEXT_PP(0));
	TupleDesc	desc;
	Datum		values[6];
	bool		nulls[6] = {false};

	SPI_connect();
	register_once();
	card_result = (CardResult) {0};
	(void) run_lines(psprintf("EXPLAIN (COSTS OFF) %s", sql), BR_CARD);
	SPI_finish();
	if (get_call_result_type(fcinfo, NULL, &desc) != TYPEFUNC_COMPOSITE)
		elog(ERROR, "sift._test_v6_card returns a row");
	values[0] = Int32GetDatum(card_result.npreds);
	values[1] = Int32GetDatum(card_result.nrest);
	values[2] = BoolGetDatum(card_result.replaced);
	values[3] = Float8GetDatum(card_result.pg_rows);
	values[4] = Float8GetDatum(card_result.rows);
	values[5] = Int32GetDatum(card_result.calls);
	PG_RETURN_DATUM(HeapTupleGetDatum(heap_form_tuple(desc, values, nulls)));
}

#endif							/* SIFT_TESTING */
