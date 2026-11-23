/*
 * agg/hybrid_exec.c - the executor of SiftHybridScan, over a sift_col index or a sift_table (its store scan decides visibility: no heap vet). Direction A: the store scan fills a bridge,
 * the ranked search of the text index reads only what the bridge holds, and each row is fetched from the heap for its tuple. Direction B: the ranked search streams hits in score order
 * into chunks of a bridge; the store scan keeps the rows of each chunk, with the columns the query reads decoded from the store (no heap fetch), and the rows leave by score (a scan that
 * is neither ranked nor reads a score takes the matches in heap order, with no search). A column the store does not hold, or a qual it did not decide for the values it got, is read from
 * the heap for the rows that survive. The steps it shares with SiftHybridAgg are in hybrid_store.c.
 *
 * check-memory: allow-allocs every array here follows one chunk of hits or one batch of rows, which the byte budget of the bridge and the granule bound
 */
#include "postgres.h"

#include "access/tableam.h"
#include "commands/explain.h"
#include "miscadmin.h"
#include "utils/builtins.h"
#include "utils/date.h"
#include "utils/memutils.h"
#include "utils/uuid.h"

#include "agg/hybrid_state.h"
#include "core/sift.h"
#include "core/memtrack.h"

/* The value of row r of a leaf as a datum of the column's type, in the current memory context; false for a row whose value the leaf did not keep */
static bool
view_datum(Oid typid, const ColView *cv, int r, Datum *out, bool *isnull)
{
	*isnull = cv->isnull != NULL && cv->isnull[r];
	if (*isnull)
	{
		*out = (Datum) 0;
		return cv->tag == NULL || cv->tag[r] == 0;
	}
	switch (typid)
	{
		case BOOLOID:
			*out = BoolGetDatum(cv->i[r] != 0);
			break;
		case INT2OID:
			*out = Int16GetDatum((int16) cv->i[r]);
			break;
		case INT4OID:
			*out = Int32GetDatum((int32) cv->i[r]);
			break;
		case DATEOID:
			*out = DateADTGetDatum((DateADT) cv->i[r]);
			break;
		case FLOAT4OID:
			*out = Float4GetDatum((float4) cv->f[r]);
			break;
		case FLOAT8OID:
			*out = Float8GetDatum(cv->f[r]);
			break;
		case TEXTOID:
		case VARCHAROID:
			*out = PointerGetDatum(cstring_to_text_with_len(cv->s[r], (int) cv->slen[r]));
			break;
		case UUIDOID:
			{
				pg_uuid_t  *u = palloc(UUID_LEN);

				memcpy(u->data, cv->s[r], UUID_LEN);
				*out = UUIDPGetDatum(u);
			}
			break;
		case TEXTARRAYOID:
		case INT4ARRAYOID:
		case INT8ARRAYOID:
			{
				void	   *img = palloc(cv->slen[r]);

				memcpy(img, cv->s[r], cv->slen[r]);
				*out = PointerGetDatum(img);
			}
			break;
		default:
			*out = Int64GetDatum(cv->i[r]);
	}
	return true;
}

static void
keep_row(HybridState *st, const SiftHybridBatch *b, int r)
{
	const ColView *views = b->cols;
	HybridRow  *row;

	if (st->nrows == st->rows_cap)
	{
		st->rows_cap = Max(64, 2 * st->rows_cap);
		st->rows = st->rows ? repalloc(st->rows, st->rows_cap * sizeof(HybridRow)) : MemoryContextAlloc(st->cxt, st->rows_cap * sizeof(HybridRow));
	}
	row = &st->rows[st->nrows++];
	row->key = b->tid[r];
	row->score = b->score ? b->score[r] : 0;
	row->need_heap = st->fetch_heap || (b->maybe != NULL && (b->maybe[r / 64] >> (r % 64) & 1));
	if (row->need_heap)
		return;
	row->vals = MemoryContextAlloc(st->chunk_cxt, st->ncols * sizeof(Datum));
	row->nulls = MemoryContextAlloc(st->chunk_cxt, st->ncols * sizeof(bool));
	for (int c = 0; c < st->ncols; c++)
		if (st->col[c].kind == HC_VAR)
		{
			MemoryContext old = MemoryContextSwitchTo(st->chunk_cxt);

			row->need_heap |= !view_datum(st->col[c].typid, &views[st->col[c].leaf], r, &row->vals[c], &row->nulls[c]);
			MemoryContextSwitchTo(old);
		}
}

static void
row_sink(void *arg, const SiftHybridBatch *b)
{
	for (int r = 0; r < b->n; r++)
		if (b->live[r / 64] >> (r % 64) & 1)
			keep_row(arg, b, r);
}

static int
row_cmp(const void *a, const void *b)
{
	const HybridRow *x = a,
			   *y = b;

	return x->score > y->score ? -1 : x->score < y->score ? 1 : x->key < y->key ? -1 : x->key > y->key;
}

/* The next match of the iteration in heap order, which carries no score */
bool
sift_match_keys_next(void *arg, SiftKey *key, float4 *score)
{
	SiftMatchKeys *mk = arg;

	for (;;)
	{
		for (; mk->word < SIFT_OFFMASK_WORDS; mk->word++)
		{
			uint64		m = mk->pm.offmask[mk->word];
			ItemPointerData tid;

			if (m == 0)
				continue;
			mk->pm.offmask[mk->word] &= m - 1;
			ItemPointerSet(&tid, mk->pm.blk, mk->word * 64 + __builtin_ctzll(m) + 1);
			*key = sift_f2_tid(&tid);
			*score = 0;
			return true;
		}
		if (!sift_iter_next(mk->it, &mk->pm))
			return false;
		mk->word = 0;
	}
}

static bool
next_chunk(HybridState *st)
{
	if (st->matches.it)
		return sift_hybrid_b_chunk(st->run, sift_match_keys_next, &st->matches, false);
	return sift_hybrid_b_chunk(st->run, sift_hybrid_next_hit, st, st->scored);
}

static void
fill_chunk(HybridState *st)
{
	MemoryContextReset(st->chunk_cxt);
	st->nrows = st->pos = 0;
	while (st->nrows == 0 && !st->chunks_done)
	{
		if (!next_chunk(st))
		{
			st->chunks_done = true;
			break;
		}
		sift_hybrid_b_emit(st->run, row_sink, st);
		if (st->rank && st->ranked)
			sift_rank_feedback(st->rank, st->run->stats.bridge_tids, st->run->stats.rows_emitted, st->k);
	}
	if (st->ranked)
		qsort(st->rows, st->nrows, sizeof(HybridRow), row_cmp);
}

/* The types the store of a sift_table hands back, and those a sift_col does */
static bool
usable_output(const HybridState *st, Oid typid)
{
	if (st->table != NULL)
		return typid == BOOLOID || typid == INT2OID || typid == INT4OID || typid == INT8OID || typid == DATEOID || typid == TIMESTAMPOID || typid == TIMESTAMPTZOID;
	return sift_hybrid_type_usable(typid) || typid == TEXTARRAYOID || typid == INT4ARRAYOID || typid == INT8ARRAYOID;
}

static void
setup_columns(HybridState *st, CustomScan *cs)
{
	int			n = 0;

	st->ncols = list_length(cs->custom_scan_tlist);
	st->col = palloc0(st->ncols * sizeof(HybridCol));
	st->leaves = palloc(st->ncols * sizeof(int));
	st->covered = true;
	foreach_node(TargetEntry, te, cs->custom_scan_tlist)
	{
		HybridCol  *c = &st->col[n++];
		bool		is_max;

		if (IsA(te->expr, Var))
		{
			Var		   *v = (Var *) te->expr;

			c->kind = v->varattno == SelfItemPointerAttributeNumber ? HC_CTID : HC_VAR;
			c->attno = v->varattno;
			c->typid = v->vartype;
			c->leaf = -1;
			if (c->kind == HC_VAR)
			{
				if (st->table != NULL)
					c->leaf = st->table->att_first[c->attno];
				else
					for (int i = 0; i < st->store_index->rd_index->indnatts; i++)
						if (st->store_index->rd_index->indkey.values[i] == c->attno)
							c->leaf = sift_col_rel(st->store_index)->first[i + 1];
				if (c->leaf >= 0 && usable_output(st, c->typid))
					st->leaves[st->nleaves++] = c->leaf;
				else
					st->covered = false;
			}
		}
		else
		{
			SIFT_INVARIANT(sift_is_score_expr((Node *) te->expr, cs->scan.scanrelid, &is_max) && !is_max, "sift: the hybrid scan outputs scores only");
			c->kind = HC_SCORE;
			st->scored = true;
		}
	}
}

static void
hybrid_begin(CustomScanState *node, EState *estate, int eflags)
{
	HybridState *st = (HybridState *) node;
	CustomScan *cs = (CustomScan *) node->ss.ps.plan;

	sift_hybrid_open(st, cs, estate, node->ss.ss_currentRelation);
	setup_columns(st, cs);
}

/* B without a score to read or an order to give: the matches in heap order, with no search, unless the plan needs the text of a hit (a phrase) */
void
sift_hybrid_unscored_plan(HybridState *st)
{
	SiftPlan   *scored = st->plan;

	st->plan = sift_plan_bind(st->ist, &st->bound_query, 1, 0);
	if (sift_plan_recheck_needed(st->plan))
		st->plan = scored;
}

static void
or_sink(void *arg, const SiftHybridBatch *b)
{
	HybridState *st = arg;

	for (int r = 0; r < b->n; r++)
		if (b->live == NULL || b->live[r / 64] >> (r % 64) & 1)
			BufFileWrite(st->or_keys, &b->tid[r], sizeof(SiftKey));
}

/* The rows the structured quals take are spooled by key, in the order of the store; the matches follow in heap order, each when the quals do not take it */
static void
start_or(HybridState *st)
{
	EState	   *estate = st->css.ss.ps.state;
	MemoryContext old = MemoryContextSwitchTo(st->cxt);

	if (sift_hybrid_bind(st))
	{
		sift_vis_begin(&st->vis, st->heap, estate->es_snapshot, st->text_index, sift_plan_dir_version(st->plan));
		st->vis_open = true;
		st->or_keys = BufFileCreateTemp(false);
		st->store.scan_quals(st->store.arg, or_sink, st);
		BufFileSeek(st->or_keys, 0, 0, SEEK_SET);
		sift_hybrid_unscored_plan(st);
		if (sift_plan_recheck_needed(st->plan))
			st->rank = sift_rank_begin(st->plan, 0, sift_hybrid_verify_rest, st);
		else
			st->matches.it = sift_iter_begin(st->plan);
	}
	MemoryContextSwitchTo(old);
}

static void
start_directed(HybridState *st)
{
	EState	   *estate = st->css.ss.ps.state;
	MemoryContext old = MemoryContextSwitchTo(st->cxt);
	bool		a;

	if (!sift_hybrid_bind(st))
	{
		MemoryContextSwitchTo(old);
		return;
	}
	if (st->adaptive)
	{
		st->dir = sift_hybrid_pick_direction(st, false);
		st->resolved = true;
	}
	else if (st->dir == SIFT_HYB_COL_TO_BM25 && !sift_hybrid_has_store_quals(st))
		st->dir = SIFT_HYB_BM25_TO_COL;
	a = st->dir == SIFT_HYB_COL_TO_BM25;
	st->point = !a && sift_plan_docs(st->plan) <= SIFT_TIDB_POINT_MAX;
	st->fetch_heap = !st->covered || !st->exact_quals;
	st->store.exact_quals = st->exact_quals;
	if (!a && !st->point && !st->ranked && !st->scored)
		sift_hybrid_unscored_plan(st);
	sift_vis_begin(&st->vis, st->heap, estate->es_snapshot, st->text_index, sift_plan_dir_version(st->plan));
	st->vis_open = true;
	st->run = sift_hybrid_begin(st->cxt, st->dir, &st->store, (Size) work_mem * 1024 / 2, a ? sift_hybrid_heap_check : sift_hybrid_visible_only, st);
	if (a)
	{
		st->bridge = sift_hybrid_a_bridge(st->run, false, sift_plan_recheck_needed(st->plan));
		if (!st->run->stats.empty)
		{
			sift_plan_set_tidbridge(st->plan, st->bridge);
			st->rank = sift_rank_begin(st->plan, st->k, sift_hybrid_verify_a, st);
			if (st->ties)
				sift_rank_ties(st->rank);
		}
	}
	else
	{
		sift_hybrid_set_limit(st->run, st->ranked ? st->k : 0);
		sift_hybrid_set_batch_check(st->run, sift_hybrid_visible_batch);
		if (st->point || st->ranked || st->scored || sift_plan_recheck_needed(st->plan))
			st->rank = sift_rank_begin(st->plan, st->k, st->point ? sift_hybrid_verify_point : sift_hybrid_verify_b, st);
		else
			st->matches.it = sift_iter_begin(st->plan);
	}
	MemoryContextSwitchTo(old);
}

static void
start_scan(HybridState *st)
{
	if (st->dir == SIFT_HYB_OR)
		start_or(st);
	else
		start_directed(st);
}

static void
store_row(HybridState *st, TupleTableSlot *slot, const HybridRow *row, ItemPointer tid, float4 score)
{
	st->out_tid = *tid;
	for (int c = 0; c < st->ncols; c++)
	{
		if (st->col[c].kind == HC_SCORE)
			slot->tts_values[c] = Float4GetDatum(score);
		else if (st->col[c].kind == HC_CTID)
			slot->tts_values[c] = PointerGetDatum(&st->out_tid);
		else if (row)
		{
			slot->tts_values[c] = row->vals[c];
			slot->tts_isnull[c] = row->nulls[c];
			continue;
		}
		else
			slot->tts_values[c] = slot_getattr(st->heapslot, st->col[c].attno, &slot->tts_isnull[c]);
		if (st->col[c].kind != HC_VAR)
			slot->tts_isnull[c] = false;
	}
	ExecStoreVirtualTuple(slot);
}

static TupleTableSlot *
next_a(HybridState *st, TupleTableSlot *slot)
{
	ItemPointerData tid;
	float4		score;
	bool		visible;

	if (st->rank == NULL || !sift_rank_next(st->rank, &tid, &score))
		return NULL;
	visible = sift_vis_fetch(&st->vis, &tid, st->heapslot);
	SIFT_INVARIANT(visible, "sift: a hit of the hybrid scan is no longer visible");
	store_row(st, slot, NULL, &tid, score);
	return slot;
}

static TupleTableSlot *
next_b(HybridState *st, TupleTableSlot *slot)
{
	const HybridRow *row;
	ItemPointerData tid;

	if (st->pos == st->nrows)
		fill_chunk(st);
	if (st->pos == st->nrows)
		return NULL;
	row = &st->rows[st->pos++];
	tid = sift_hybrid_tid_of(row->key);
	if (row->need_heap && !sift_vis_fetch(&st->vis, &tid, st->heapslot))
		return next_b(st, slot);
	store_row(st, slot, row->need_heap ? NULL : row, &tid, row->score);
	return slot;
}

static TupleTableSlot *
next_or_key(HybridState *st, TupleTableSlot *slot)
{
	SiftKey		key;

	while (BufFileReadMaybeEOF(st->or_keys, &key, sizeof(key), true))
		if (sift_hybrid_heap_check(st, key, true))
		{
			store_row(st, slot, NULL, &st->fetched_tid, 0);
			return slot;
		}
	st->or_matches = true;
	return NULL;
}

static TupleTableSlot *
next_or_match(HybridState *st, TupleTableSlot *slot)
{
	SiftKey		key;
	float4		score;
	ItemPointerData tid;

	if (st->rank)
		return next_a(st, slot);
	while (sift_match_keys_next(&st->matches, &key, &score))
	{
		tid = sift_hybrid_tid_of(key);
		if (sift_hybrid_rest_row(st, &tid))
		{
			store_row(st, slot, NULL, &tid, 0);
			return slot;
		}
	}
	return NULL;
}

static TupleTableSlot *
next_or(HybridState *st, TupleTableSlot *slot)
{
	TupleTableSlot *keyed = st->or_matches ? NULL : next_or_key(st, slot);

	return keyed ? keyed : next_or_match(st, slot);
}

static TupleTableSlot *
hybrid_next(ScanState *ss)
{
	HybridState *st = (HybridState *) ss;
	TupleTableSlot *slot = ss->ss_ScanTupleSlot;

	if (!st->started)
		start_scan(st);
	ExecClearTuple(slot);
	CHECK_FOR_INTERRUPTS();
	if (st->empty)
		return NULL;
	if (st->dir == SIFT_HYB_OR)
		return next_or(st, slot);
	return st->dir == SIFT_HYB_COL_TO_BM25 || st->point ? next_a(st, slot) : next_b(st, slot);
}

static bool
hybrid_recheck(ScanState *ss, TupleTableSlot *slot)
{
	return true;
}

static TupleTableSlot *
hybrid_exec(CustomScanState *node)
{
	return ExecScan(&node->ss, hybrid_next, hybrid_recheck);
}

static void
hybrid_rescan(CustomScanState *node)
{
	HybridState *st = (HybridState *) node;

	sift_hybrid_finish(st);
	MemoryContextReset(st->cxt);
	st->nrows = st->pos = st->rows_cap = 0;
	st->rows = NULL;
	st->chunks_done = st->or_matches = false;
	st->granules_seen = st->granules_skipped = st->parts_skipped = st->point_verifies = 0;
	st->empty = false;
	ExecScanReScan(&node->ss);
}

static void
hybrid_end(CustomScanState *node)
{
	HybridState *st = (HybridState *) node;

	sift_hybrid_finish(st);
	sift_hybrid_close(st);
}

static void
hybrid_explain(CustomScanState *node, List *ancestors, ExplainState *es)
{
	HybridState *st = (HybridState *) node;
	const SiftHybridStats *s = st->started && st->run ? &st->run->stats : &st->stats;

	ExplainPropertyText("Hybrid Direction", st->adaptive ? (st->resolved ? psprintf("%s%s (adaptive)", sift_hybrid_dir_name(st->dir), st->point ? " point" : "") : "adaptive") : sift_hybrid_dir_name(st->dir), es);
	ExplainPropertyText("Hybrid Store", st->table != NULL ? "sift_table" : "sift_col", es);
	if (st->ext != NIL)
	{
		StringInfoData buf;
		static const char *const names[] = {"", "IPREFIX", "IEQ", "ARR_OVERLAP", "ARR_CONTAINS", "CARD", "NE"};

		initStringInfo(&buf);
		foreach_ptr(SiftHybExt, e, st->ext)
			appendStringInfo(&buf, "%s%s", buf.len > 0 ? ", " : "", names[e->qual.kind]);
		ExplainPropertyText("Extended Predicates", buf.data, es);
	}
	if (!es->analyze)
		return;
	ExplainPropertyInteger("Bridge Chunks", NULL, s->chunks, es);
	ExplainPropertyInteger("Bridge TIDs", NULL, s->bridge_tids, es);
	ExplainPropertyInteger("Bridge Exact Pages", NULL, s->pages_exact, es);
	ExplainPropertyInteger("Bridge Lossy Pages", NULL, s->pages_lossy, es);
	ExplainPropertyInteger("Granules Skipped", NULL, s->granules_skipped, es);
	ExplainPropertyInteger("Granules Seen", NULL, s->granules_seen, es);
	if (st->parts_skipped > 0)
		ExplainPropertyInteger("Parts Skipped", NULL, st->parts_skipped, es);
	ExplainPropertyInteger("Rows Probed", NULL, s->rows_probed, es);
	ExplainPropertyInteger("Rows Emitted", NULL, st->point ? st->point_verifies : st->dir == SIFT_HYB_BM25_TO_COL ? s->rows_emitted : s->zero_heap_rows + s->heap_verifies, es);
	if (st->dir == SIFT_HYB_COL_TO_BM25)
	{
		ExplainPropertyBool("Bridge Point", s->point, es);
		ExplainPropertyBool("Bridge Empty", s->empty, es);
		ExplainPropertyInteger("Heap Verifies", NULL, s->heap_verifies, es);
		ExplainPropertyInteger("Lossy Rechecks", NULL, s->lossy_rechecks, es);
	}
}

static const CustomExecMethods hybrid_exec_methods = {
	.CustomName = "SiftHybridScan",
	.BeginCustomScan = hybrid_begin,
	.ExecCustomScan = hybrid_exec,
	.EndCustomScan = hybrid_end,
	.ReScanCustomScan = hybrid_rescan,
	.ExplainCustomScan = hybrid_explain,
};

static Node *
hybrid_create_state(CustomScan *cscan)
{
	HybridState *st = (HybridState *) newNode(sizeof(HybridState), T_CustomScanState);

	st->css.methods = &hybrid_exec_methods;
	return (Node *) st;
}

const CustomScanMethods sift_hybrid_plan_methods = {"SiftHybridScan", hybrid_create_state};
