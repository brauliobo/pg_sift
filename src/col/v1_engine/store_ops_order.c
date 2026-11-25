/*
 * col/v1_engine/store_ops_order.c - SiftOrderOps of Format 1 (core/store/ops.h). The files of col/v1_engine/ are the part of the Format 1 store that reads the engine's headers
 * (agg/), which a file of col/v1/ may not include (scripts/check-format-isolation.sh); they move into col/v1/ once those headers are format-neutral.
 *
 * A Format 1 source gives the rows of its units as they are stored, so no prefix of attributes is an order of its batches (sorted_by is false and a sorted source is never asked for);
 * the lead key is the attribute its keyed units are cut by. scan_rows keeps the best rows of an ORDER BY .. LIMIT of one relation: from the global order of the first sort column when the
 * index keeps one (v1_gorder_cursor.h), whose first rows the heap confirms, else by folding the batches of its source (col/source_fold.h) into the best rows, with the quals the source
 * does not run decided on the decoded batch, the aggregates of the whole set and the chunks of a text match when the statement asks; the winners are the tags (TIDs) of the rows, which
 * the node fetches from the heap in order.
 */
/* check-memory: allow-allocs every array here follows the columns of one scan or the rows of one batch, not the rows of the relation */
#include "postgres.h"

#include "access/genam.h"
#include "access/table.h"
#include "access/tableam.h"
#include "catalog/pg_type_d.h"
#include "executor/executor.h"
#include "nodes/nodeFuncs.h"
#include "nodes/primnodes.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/rel.h"

#include "agg/engine.h"
#include "agg/topn/rows.h"
#include "agg/topn/topn.h"
#include "agg/vexpr.h"
#include "col/source_fold.h"
#include "col/v1/gorder/gorder.h"
#include "col/v1/reader.h"
#include "col/v1/store_ops.h"
#include "col/v1_engine/gorder_cursor.h"
#include "core/compat.h"
#include "core/memtrack.h"
#include "core/sift.h"
#include "table/v1/rel.h"

static bool
order_sorted_by(Oid relid, bool is_table, const int *atts, int n)
{
	return false;
}

static int
order_lead_att(Oid relid, bool is_table)
{
	SiftV1Rel	r;
	const SiftV1ReaderOps *rd;
	int			leaf,
				att;

	if (is_table)
		return 0;
	r = sift_v1_rel_open(relid, false);
	rd = sift_v1_reader_of(false);
	leaf = rd->key_leaf(r.rel);
	att = leaf < 0 ? 0 : rd->layout(r.rel)->leaf[leaf].attno;
	sift_v1_rel_close(&r);
	return att;
}

/* ---- the row Top-N with what the batches of the source are folded for: keys computed from columns, the aggregates of the whole set, a text match ---- */

typedef struct RowsFold
{
	SiftV6Rows *rows;
	SiftV6Group *totals;
	const SiftSrcFold *fold;
} RowsFold;

static void
rows_emit(void *arg, const SiftBatchSel *bs, SiftVec *const *keyvec, SiftVec *const *argvec)
{
	const RowsFold *f = arg;

	sift_v6_rows_update(f->rows, bs, keyvec, sift_srcfold_tags(f->fold));
	if (f->totals != NULL)
		sift_v6_group_update(f->totals, bs, keyvec, argvec);
}

typedef struct RowsOpen
{
	MemoryContext cx;
	Snapshot	snap;
	SiftV1Scan	scan;
} RowsOpen;

static SiftV6Source *
rows_open(void *arg, const SiftV6Input *in, const SiftV6Vexpr *quals)
{
	const RowsOpen *o = arg;

	return sift_v1_source_open(o->cx, o->snap, in, quals, &o->scan);
}

static void
rows_fold(const SiftV6Spec *spec, SiftV6Rows *rows, SiftV6Group *totals, Relation heap, Relation index, ScanKey keys, int nkeys, const struct SiftStoreQuals *quals, Snapshot snap,
		  SiftV6ScanStats *stats, SiftV6ChunkFn next_chunk, void *chunk_arg)
{
	MemoryContext cx = sift_mem_allocset_create(CurrentMemoryContext, "sift v1 rows");
	RowsOpen	o = {.cx = cx,.snap = snap,.scan = spec->is_table ? (SiftV1Scan) {0} : sift_v1_key_scan(index, keys, nkeys)};
	RowsFold	emit = {.rows = rows,.totals = totals};
	SiftSrcFoldArgs a = {.spec = spec,.stats = stats,.emit = rows_emit,.emit_arg = &emit,.host = spec->is_table ? heap : index,.is_table = spec->is_table,
		.quals = spec->is_table ? quals->clauses : sift_srcfold_scankey_quals(index, keys, nkeys),.estate = spec->is_table ? quals->estate : NULL};
	SiftSrcFold *f = sift_srcfold_new(&a, sift_mem_allocset_create(cx, "sift v1 rows fold"));
	const struct SiftTidBridge *bridge;

	emit.fold = f;
	if (next_chunk == NULL)
		sift_srcfold_run(f, rows_open, &o);
	while (next_chunk != NULL && next_chunk(chunk_arg, &bridge))
	{
		sift_srcfold_count_chunk(stats, bridge);
		o.scan.bridge = bridge;
		sift_srcfold_run(f, rows_open, &o);
	}
	MemoryContextDelete(cx);
}

/* ---- the Top-N over the global order of the first sort column ---- */

/* the entries the walk examines at most: a qual that few rows pass, or an order of dead rows, is read faster from the units */
#define GORD_SCAN_MIN	4096
#define GORD_SCAN_MULT	64

/* What the two Top-N entries of the store (the rows of scan_rows, the tournament of topn_open) tell of their sort: the columns of the index, the direction of the first, the rows they want */
typedef struct GOrdShape
{
	int			nkeys;
	const SiftV6Col *key;
	bool		desc;
	bool		nulls_first;
	uint32		rows;
} GOrdShape;

typedef struct GOrdPlan
{
	SiftV1GOrdSpec cur;
	int			nkeys;
	AttrNumber	att[SIFT_V6_MAX_KEYS];	/* the attribute of the heap of each sort column */
	Oid			typid[SIFT_V6_MAX_KEYS];
	ExprState  *quals;
	ExprContext *econtext;
	bool		ties;			/* more sort columns than the first: the rows that tie on it are all read */
} GOrdPlan;

/* the rows the walk found, before the Top-N takes them: a walk that cannot answer leaves the Top-N as it was */
typedef struct GOrdWinners
{
	int			nkeys;
	uint32		n;
	uint32		room;
	Datum	   *val;
	bool	   *nul;
	uint64	   *tid;
} GOrdWinners;

static bool
int_key_type(Oid typid)
{
	return typid == INT2OID || typid == INT4OID || typid == INT8OID || typid == DATEOID || typid == TIMESTAMPOID || typid == TIMESTAMPTZOID;
}

/* a clause the heap row alone decides: its columns are attributes of the table, it takes no parameter and no subquery */
static bool
heap_row_clause(Node *n, void *natts)
{
	if (n == NULL)
		return true;
	if (IsA(n, Var))
		return ((Var *) n)->varno == 1 && ((Var *) n)->varlevelsup == 0 && ((Var *) n)->varattno > 0 && ((Var *) n)->varattno <= *(int *) natts;
	if (IsA(n, Param) || IsA(n, SubLink) || IsA(n, SubPlan))
		return false;
	return expression_tree_walker(n, heap_row_clause, natts);
}

static bool
json_key(const ScanKeyData *k)
{
	return k->sk_strategy == COL_STRAT_JSONB_EXISTS || k->sk_strategy == COL_STRAT_JSONB_TYPE;
}

static bool
gorder_quals(MemoryContext cx, const SiftV6Spec *spec, Relation heap, Relation index, ScanKey keys, int nkeys, GOrdPlan *p)
{
	List	   *clauses;
	int			natts = RelationGetNumberOfAttributes(heap);

	for (int i = 0; i < nkeys; i++)
		if (json_key(&keys[i]))
			return false;
	clauses = list_concat(sift_srcfold_scankey_quals(index, keys, nkeys), list_copy(spec->vqual));
	foreach_ptr(Node, q, clauses)
		if (!heap_row_clause(q, &natts))
			return false;
	if (clauses != NIL)
	{
		MemoryContext old = MemoryContextSwitchTo(cx);

		fix_opfuncids((Node *) clauses);
		p->quals = ExecInitQual(clauses, NULL);
		p->econtext = CreateStandaloneExprContext();
		MemoryContextSwitchTo(old);
	}
	return true;
}

/* the sort columns are plain columns of the index whose heap values are the Datums of an int64 key, the first of a type the order of its key is the order of */
static bool
gorder_plan(const GOrdShape *sh, Relation heap, Relation index, Snapshot snap, GOrdPlan *p)
{
	const SiftSrcLayout *lay = sift_v1_reader_of(false)->layout(index);
	const SiftSrcLeaf *leaf;

	*p = (GOrdPlan) {.nkeys = sh->nkeys,.ties = sh->nkeys > 1};
	if (sh->rows == 0 || sh->rows > SIFT_CORDER_MAX_ROWS)
		return false;
	for (int i = 0; i < sh->nkeys; i++)
	{
		if (sh->key[i].attno > index->rd_index->indnatts || !int_key_type(sh->key[i].typid))
			return false;
		p->att[i] = index->rd_index->indkey.values[sh->key[i].attno - 1];
		p->typid[i] = sh->key[i].typid;
		if (p->att[i] == 0)
			return false;
	}
	leaf = sift_v1_att_leaf(lay, sh->key[0].attno);
	if (leaf == NULL)
		return false;
	p->cur = (SiftV1GOrdSpec) {.heap = heap,.index = index,.snapshot = snap,.leaf = leaf - lay->leaf,.heap_att = p->att[0],.typid = p->typid[0],.desc = sh->desc,
		.nulls_first = sh->nulls_first,.rows = sh->rows,.budget = Max(GORD_SCAN_MIN, (uint64) GORD_SCAN_MULT * sh->rows)};
	return true;
}

static int64
key_of(Oid typid, Datum d)
{
	return typid == INT2OID ? DatumGetInt16(d) : typid == INT4OID || typid == DATEOID ? DatumGetInt32(d) : DatumGetInt64(d);
}

static Datum
key_datum(Oid typid, int64 key)
{
	return typid == INT2OID ? Int16GetDatum((int16) key) : typid == INT4OID || typid == DATEOID ? Int32GetDatum((int32) key) : Int64GetDatum(key);
}

/* false: the row fails the quals of the scan */
static bool
winner_add(GOrdWinners *w, const GOrdPlan *p, SiftV1GOrd *g, SiftV1GRow *row)
{
	TupleTableSlot *slot = NULL;
	Datum	   *val;
	bool	   *nul;

	if (p->quals != NULL || p->ties)
	{
		slot = sift_v1_gord_row(g, row);
		if (p->quals != NULL)
		{
			p->econtext->ecxt_scantuple = slot;
			ResetExprContext(p->econtext);
			if (!ExecQual(p->quals, p->econtext))
				return false;
		}
	}
	if (w->n == w->room)
	{
		w->room = Max(w->room * 2, 64);
		w->val = w->val == NULL ? palloc(w->room * w->nkeys * sizeof(Datum)) : repalloc(w->val, w->room * w->nkeys * sizeof(Datum));
		w->nul = w->nul == NULL ? palloc(w->room * w->nkeys * sizeof(bool)) : repalloc(w->nul, w->room * w->nkeys * sizeof(bool));
		w->tid = w->tid == NULL ? palloc(w->room * sizeof(uint64)) : repalloc(w->tid, w->room * sizeof(uint64));
	}
	val = w->val + (size_t) w->n * w->nkeys;
	nul = w->nul + (size_t) w->n * w->nkeys;
	for (int j = 0; j < w->nkeys; j++)
		if (slot != NULL)
			val[j] = slot_getattr(slot, p->att[j], &nul[j]);
		else
		{
			nul[j] = row->isnull;
			val[j] = row->isnull ? (Datum) 0 : key_datum(p->typid[0], row->key);
		}
	w->tid[w->n++] = sift_key_from_tid(&row->tid);
	return true;
}

/*
 * The first rows of the order, and with more sort columns every row that ties the last on the first (the rows that sort first among them are not the order's first). The winners are
 * the order's own while the walk ended inside it: after the rows wanted (or the rows that tie the last), or at the end of every source; where it ran out of budget or of a segment's ends it says nothing.
 */
static bool
gorder_walk(SiftV1GOrd *g, const GOrdPlan *p, uint32 rows, GOrdWinners *w)
{
	SiftV1GRow	row;
	int64		last = 0;
	bool		last_null = false,
				done = false;

	while (!(w->n >= rows && !p->ties) && sift_v1_gord_next(g, &row))
	{
		if (w->n >= rows && (row.isnull != last_null || row.key != last))
		{
			done = true;
			break;
		}
		if (winner_add(w, p, g, &row))
		{
			last = row.key;
			last_null = row.isnull;
		}
	}
	done |= w->n >= rows && !p->ties;
	return !sift_v1_gord_over(g) && (done || !sift_v1_gord_short(g));
}

/* the winners of the order the stored segments and the write buffers give, as the snapshot sees them, in cx; false: the order cannot answer, and nothing was kept */
static bool
gorder_winners(MemoryContext cx, const GOrdPlan *p, uint32 rows, GOrdWinners *w, uint64 *examined)
{
	SiftV1GOrd *g = sift_v1_gord_open(cx, &p->cur);
	MemoryContext old = MemoryContextSwitchTo(cx);
	bool		answered = false;

	*w = (GOrdWinners) {.nkeys = p->nkeys};
	if (g != NULL)
	{
		answered = gorder_walk(g, p, rows, w);
		*examined = sift_v1_gord_examined(g);
		sift_v1_gord_close(g);
	}
	MemoryContextSwitchTo(old);
	return answered;
}

/* ---- the rows of scan_rows ---- */

static bool
gorder_rows(const SiftV6Spec *spec, SiftV6Rows *rows, Relation heap, Relation index, ScanKey keys, int nkeys, Snapshot snap, SiftV6ScanStats *stats)
{
	const SiftV6TopKItem *item = &spec->topk_item[0];
	MemoryContext cx = sift_mem_allocset_create(CurrentMemoryContext, "sift v1 gorder");
	Oid			opfamily,
				opcintype;
	SiftCmpStrategy cmptype;
	GOrdShape	sh = {.nkeys = spec->nkeys,.key = spec->key,.nulls_first = item->nulls_first,.rows = spec->topk};
	GOrdPlan	p;
	GOrdWinners w = {0};
	uint64		examined = 0;
	bool		answered;

	get_ordering_op_properties(item->sortop, &opfamily, &opcintype, &cmptype);
	sh.desc = cmptype == SIFT_CMP_GT;
	answered = !spec->is_table && gorder_plan(&sh, heap, index, snap, &p) && gorder_quals(cx, spec, heap, index, keys, nkeys, &p) &&
		gorder_winners(cx, &p, spec->topk, &w, &examined);
	for (uint32 i = 0; answered && i < w.n; i++)
		sift_v6_rows_put(rows, w.val + (size_t) i * w.nkeys, w.nul + (size_t) i * w.nkeys, w.tid[i]);
	stats->batches += answered;
	stats->rows += examined;
	MemoryContextDelete(cx);
	return answered;
}

/* ---- the tournament of topn_open: one unit that holds the winners of the order ---- */

typedef struct TopNSrc
{
	MemoryContext cx;
	Relation	heap;
	Snapshot	snapshot;
	GOrdPlan	plan;
	GOrdWinners w;
	IndexFetchTableData *fetch;
	TupleTableSlot *slot;
} TopNSrc;

static void
topn_seed(void *src, SiftTournament *t)
{
	const SiftTournBound unknown = {0};

	sift_tourn_push(t, STOK_GRANULE, 0, 0, 0, &unknown, NULL);
}

static void
topn_scan(void *src, SiftTournament *t, const SiftTopNSchedTok *tok)
{
	const TopNSrc *s = src;

	for (uint32 i = 0; i < s->w.n; i++)
	{
		SiftTopNWinRow row = {.tid = s->w.tid[i]};

		for (int j = 0; j < s->w.nkeys; j++)
		{
			const size_t at = (size_t) i * s->w.nkeys + j;

			row.nulls |= (uint8) (s->w.nul[at] << j);
			row.key[j] = s->w.nul[at] ? 0 : key_of(s->plan.typid[j], s->w.val[at]);
		}
		sift_tourn_offer(t, &row);
	}
}

static void
topn_unused(void *src, SiftTournament *t, const SiftTopNSchedTok *tok)
{
	SIFT_INVARIANT(false, "the Top-N source of a Format 1 index has one unit, which is read");
}

static bool
topn_fetch(void *src, const SiftTopNWinRow *w, const AttrNumber *attnos, int n, Datum *values, bool *isnull)
{
	const TopNSrc *s = src;
	ItemPointerData tid;
	bool		again = false,
				dead;

	sift_key_to_tid(w->tid, &tid);
	if (!table_index_fetch_tuple(s->fetch, &tid, s->snapshot, s->slot, &again, &dead))
		return false;
	for (int i = 0; i < n; i++)
		values[i] = slot_getattr(s->slot, attnos[i], &isnull[i]);
	return true;
}

static void
topn_close(void *src)
{
	TopNSrc    *s = src;

	table_index_fetch_end(s->fetch);
	ExecDropSingleTupleTableSlot(s->slot);
	table_close(s->heap, NoLock);
	MemoryContextDelete(s->cx);
}

/* false: the order cannot answer this run (a qual the heap rows decide, a segment that lacks the order, a walk that reads more than the units): the node sorts the rows of the source */
static bool
order_topn_open(EState *estate, const SiftV6TopNSpec *spec, const SiftV6Input *in, const SiftV6Vexpr *quals, SiftV6TopNSource *dst)
{
	const GOrdShape sh = {.nkeys = spec->nkeys,.key = spec->key,.desc = spec->order[0].desc,.nulls_first = spec->order[0].nulls_first,.rows = (uint32) Min(spec->rows, SIFT_CORDER_MAX_ROWS + 1)};
	MemoryContext cx = sift_mem_allocset_create(estate->es_query_cxt, "sift v1 topn");
	Relation	index = index_open(spec->indexoid, AccessShareLock);
	Relation	heap = table_open(index->rd_index->indrelid, AccessShareLock);
	TopNSrc    *s = MemoryContextAllocZero(cx, sizeof(TopNSrc));
	uint64		examined;

	if (quals != NULL || !gorder_plan(&sh, heap, index, estate->es_snapshot, &s->plan) || !gorder_winners(cx, &s->plan, sh.rows, &s->w, &examined))
	{
		table_close(heap, NoLock);
		index_close(index, NoLock);
		MemoryContextDelete(cx);
		return false;
	}
	s->cx = cx;
	s->heap = heap;
	s->snapshot = estate->es_snapshot;
	s->fetch = sift_table_index_fetch_begin(heap);
	s->slot = table_slot_create(heap, NULL);
	index_close(index, NoLock);
	*dst = (SiftV6TopNSource) {.ops = {.expand = topn_unused,.frontier_next = topn_unused,.scan = topn_scan},.src = s,.seed = topn_seed,.fetch = topn_fetch,.close = topn_close};
	return true;
}

static void
order_scan_rows(const SiftV6Spec *spec, SiftV6Rows *rows, SiftV6Group *totals, struct SiftV6StarRun *star, Relation heap, Relation index, ScanKey keys, int nkeys,
				const struct SiftStoreQuals *quals, Snapshot snap, SiftV6ScanStats *stats, SiftV6ChunkFn next_chunk, void *chunk_arg)
{
	SIFT_INVARIANT(star == NULL, "the row Top-N of a Format 1 relation %u reads no dimensions of a join", spec->relid);
	if (totals == NULL && next_chunk == NULL && spec->nderive == 0 && gorder_rows(spec, rows, heap, index, keys, nkeys, snap, stats))
		return;
	rows_fold(spec, rows, totals, heap, index, keys, nkeys, quals, snap, stats, next_chunk, chunk_arg);
}

const SiftOrderOps v1_order_ops = {
	.sorted_by = order_sorted_by,
	.lead_att = order_lead_att,
	.scan_rows = order_scan_rows,
	.rows_serves = SIFT_ROWS_TOTALS | SIFT_ROWS_DERIVED | SIFT_ROWS_TEXT,
	.topn_open = order_topn_open
};
