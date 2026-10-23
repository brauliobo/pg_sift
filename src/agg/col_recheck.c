/*
 * agg/col_recheck.c - the recheck of the rows that hold a value a leaf cannot store, read again from the heap
 */
#include "postgres.h"

#include "agg/agg.h"

#if __has_include("col/col.h")

#include "access/genam.h"
#include "access/relation.h"
#include "access/table.h"
#include "access/tableam.h"
#include "catalog/index.h"
#include "catalog/pg_type.h"
#include "executor/executor.h"
#include "nodes/execnodes.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/rel.h"

#include "agg/kern.h"
#include "agg/pre.h"
#include "agg/sel.h"
#include "agg/wide.h"
#include "core/popcount.h"
#include "col/col.h"
#include "col/gpu.h"
#include "col/pagg.h"
#include "col/am.h"
#include "col/enc.h"
#include "col/pg.h"
#include "agg/colsrc.h"
#include "col/shred_pg.h"

#include "agg/col_int.h"

/* ---- the recheck of the rows that hold a value a leaf cannot store ---- */

static ColRecheck *
recheck_init(ColSource *src)
{
	MemoryContext old = MemoryContextSwitchTo(src->cxt);
	ColRecheck *k = palloc0(sizeof(ColRecheck));
	/* check-memory: allow r->shred is read once, before any lock or catalog read */
	SiftColRel *r = sift_col_rel(src->index);

	k->heap = src->heap;
	k->index = src->index;
	k->snap = src->snapshot;
	k->life = src->cxt;
	k->cxt = AllocSetContextCreate(src->cxt, "sift recheck rows", ALLOCSET_DEFAULT_SIZES);
	k->tmp = AllocSetContextCreate(src->cxt, "sift recheck row", ALLOCSET_DEFAULT_SIZES);
	k->rc = sift_col_shred_recheck_create(src->cxt, r->shred);
	k->rc->mcxt = k->cxt;
	src->rck = k;
	MemoryContextSwitchTo(old);
	return k;
}

void
col_set_recheck(void *impl, int leaf, int mode, const char *key, Oid cast)
{
	ColSource  *src = impl;
	ColRecheck *k = src->rck ? src->rck : recheck_init(src);
	/* check-memory: allow r->shred and r->first are read for the arguments of the calls, r is not used after them */
	SiftColRel *r = sift_col_rel(src->index);
	int			att = src->lay->leaf[leaf].attno;

	if (mode == SRK_NONE)
		return;
	if (key != NULL)
		sift_col_shred_recheck_member(k->rc, r->shred, att, leaf - r->first[att], key, cast);
	else
		sift_col_shred_recheck_set(k->rc, r->shred, att, leaf - r->first[att], mode == SRK_RECORDSET ? SRG_RECORDSET : mode == SRK_CAST ? SRC_JSONCAST : SRC_TEXTIO);
	for (int i = 0; i < k->nleaf; i++)
		if (k->leaf[i] == leaf)
			return;
	k->leaf[k->nleaf++] = leaf;
}

/* the rows whose slot of the leaf overflowed, an element of a child leaf counting for its row */
static void
recheck_flag_leaf(ColSource *src, int leaf, uint64 *rows)
{
	const SiftColUnit *cu = &src->unit;
	uint32		n;
	const uint64 *ovf = sift_col_ovf_slots(cu, leaf, &n);
	int			parent = src->lay->leaf[leaf].parent;
	const uint32 *par = NULL,
			   *counts;

	if (ovf == NULL)
		return;
	if (parent >= 0)
		sift_col_group_parents(cu, parent, &par, &counts);
	for (uint32 w = 0; w < (n + 63) / 64; w++)
		for (uint64 b = ovf[w]; b; b &= b - 1)
		{
			uint32		e = w * 64 + pg_rightmost_one_pos64(b),
						r = par ? par[e] : e;

			rows[r >> 6] |= UINT64CONST(1) << (r & 63);
		}
}

/*
 * Called by `live` for the stored unit the source is at: the rows that hold such a value and are live leave `bits` and become the candidates
 * (k->cand, which the executor may narrow, col_recheck_rows), read again from the heap by the next call of col_next (recheck_pending).
 * Once per unit: the flagged rows are kept, a second call only takes them out of its bits again.
 */
void
recheck_collect(ColSource *src, uint32 nrow, uint64 *bits)
{
	ColRecheck *k = src->rck;
	uint32		nw = SIFT_COL_NWORDS(nrow);

	if (k->gen != src->ugen)
	{
		bool		any = false;

		k->gen = src->ugen;
		k->taken = false;
		k->nrow = nrow;
		k->flag = k->flag ? repalloc(k->flag, Max(nw, 1) * sizeof(uint64)) : MemoryContextAlloc(src->cxt, Max(nw, 1) * sizeof(uint64));
		k->cand = k->cand ? repalloc(k->cand, Max(nw, 1) * sizeof(uint64)) : MemoryContextAlloc(src->cxt, Max(nw, 1) * sizeof(uint64));
		memset(k->flag, 0, Max(nw, 1) * sizeof(uint64));
		for (int i = 0; i < k->nleaf; i++)
			if (col_overflow(src, NULL, k->leaf[i]))
			{
				recheck_flag_leaf(src, k->leaf[i], k->flag);
				any = true;
			}
		if (!any)
			return;
	}
	if (!k->taken)
	{
		uint64		any = 0;

		for (uint32 w = 0; w < nw; w++)
			any |= k->cand[w] = k->flag[w] & bits[w];
		k->taken = any != 0;
	}
	for (uint32 w = 0; w < nw; w++)
		bits[w] &= ~k->flag[w];
}

uint64 *
col_recheck_rows(void *impl, const SiftSrcUnit *unit)
{
	ColSource  *src = impl;

	/* the rows of a match set are renumbered: the executor does not test them */
	return src->rck && src->rck->taken && !src->sel.keep ? src->rck->cand : NULL;
}

/* the TIDs of the candidates the executor left, to be the rows of the next unit; false: none is left */
bool
recheck_pending(ColSource *src)
{
	ColRecheck *k = src->rck;
	uint32		nw = SIFT_COL_NWORDS(k->nrow),
				m = 0;
	uint32	   *ords;

	for (uint32 v = 0; v < nw; v++)
		m += pg_popcount64(k->cand[v]);
	if (m == 0)
		return false;
	ords = palloc(m * sizeof(uint32));
	m = 0;
	for (uint32 v = 0; v < nw; v++)
		for (uint64 b = k->cand[v]; b; b &= b - 1)
			ords[m++] = v * 64 + pg_rightmost_one_pos64(b);
	k->tid = k->tid ? repalloc(k->tid, m * sizeof(ItemPointerData)) : MemoryContextAlloc(src->cxt, m * sizeof(ItemPointerData));
	sift_col_row_tids(&src->unit, ords, m, k->tid);
	pfree(ords);
	k->npend = m;
	k->sb = src->unit.sb;
	k->nunits++;
	return true;
}

/* the tuple the TID names for the snapshot (the version an index scan would find, following a HOT chain), in k->slot; false: no version is visible */
static bool
recheck_fetch(ColRecheck *k, ItemPointer tid)
{
	bool		call_again = false,
				all_dead;

	if (k->slot == NULL)
	{
		MemoryContext old = MemoryContextSwitchTo(k->life);

		k->fetch = sift_table_index_fetch_begin(k->heap);
		k->slot = table_slot_create(k->heap, NULL);
		k->estate = CreateExecutorState();
		k->info = BuildIndexInfo(k->index);
		MemoryContextSwitchTo(old);
	}
	ExecClearTuple(k->slot);
	return table_index_fetch_tuple(k->fetch, tid, k->snap, k->slot, &call_again, &all_dead);
}

/* the rows of the TIDs read again from the heap, with the values of the document as the query's SQL gives them, as the batch of the unit that follows; NULL: none is visible */
static SiftColBatch *
recheck_batch(ColSource *src)
{
	ColRecheck *k = src->rck;
	SiftColBatch *b;
	uint32		n = 0;

	MemoryContextReset(k->cxt);
	b = sift_cbatch_create(k->cxt, sift_col_schema(src->cs));
	sift_cbatch_reset(b, k->sb);
	k->err = MemoryContextAllocZero(k->cxt, Max(k->npend, 1) * sizeof(ErrorData *));
	for (uint32 i = 0; i < k->npend; i++)
	{
		Datum		values[INDEX_MAX_KEYS];
		bool		isnull[INDEX_MAX_KEYS];
		SiftColRow	row;
		MemoryContext old;
		ItemPointerData visible;

		CHECK_FOR_INTERRUPTS();
		/* the fetch moves the TID to the visible version of a HOT chain: the row keeps the index's own, the root's */
		visible = k->tid[i];
		if (!recheck_fetch(k, &visible))
			continue;
		GetPerTupleExprContext(k->estate)->ecxt_scantuple = k->slot;
		old = MemoryContextSwitchTo(k->tmp);
		FormIndexDatum(k->info, k->slot, k->estate, values, isnull);
#ifdef SIFT_TESTING
		sift_col_test_inval(k->index);
#endif
		/* the index's cached state is freed when its relcache entry is reloaded, which a lock taken by the fetch or the index's expressions may cause: not kept across tuples */
		sift_col_make_row_rc(sift_col_rel(k->index), &k->tid[i], values, isnull, &row, k->tmp, k->rc);
		sift_cbatch_add(b, &row);
		MemoryContextSwitchTo(old);
		k->err[n++] = k->rc->err;
		ResetPerTupleExprContext(k->estate);
		MemoryContextReset(k->tmp);
	}
	ExecClearTuple(k->slot);
	k->npend = 0;
	k->nrechecked += n;
	return n > 0 ? b : NULL;
}

/* the unit that follows a stored one that had rows to recheck; false: none of them is visible */
bool
recheck_next(ColSource *src, SiftSrcUnit *unit)
{
	SiftColBatch *b = recheck_batch(src);

	if (b == NULL)
		return false;
	sift_col_unit_batch(src->cs, b, &src->unit);
	src->ugen++;
	src->rck->active = true;
	unit->sb = src->unit.sb;
	unit->nrow = src->unit.nrows;
	unit->nsu = 0;
	unit->priv = src;
	return true;
}

void
col_overflow_raise(void *impl, const SiftSrcUnit *unit, const uint64 *rows)
{
	ColSource  *src = impl;
	ColRecheck *k = src->rck;

	if (k == NULL || !k->active)
		return;
	for (uint32 r = 0; r < unit->nrow; r++)
		if (sift_colsrc_bit(rows, r) && k->err[r])
			ThrowErrorData(k->err[r]);
}

void
recheck_unpin(ColRecheck *k)
{
	if (k->slot)
	{
		ExecClearTuple(k->slot);
		table_index_fetch_reset(k->fetch);
	}
}

void
recheck_close(ColRecheck *k)
{
	if (k->slot)
	{
		table_index_fetch_end(k->fetch);
		ExecDropSingleTupleTableSlot(k->slot);
		FreeExecutorState(k->estate);
	}
}

#endif							/* __has_include("col/col.h") */
