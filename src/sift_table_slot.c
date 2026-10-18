/*
 * sift_table_slot.c - the tuple table slot of a sift_table (docs/COL-TAM.md section 4): a virtual slot whose attributes of a row of a column
 * group are decoded when an expression asks for them. A scan or a fetch only records where the row is (the group scan, the unit token,
 * the position in the unit's vectors); slot_getsomeattrs reaches getsomeattrs with the number of leading attributes the query reads, and
 * only the columns up to there are decoded, one vector per column and unit, on the first row that needs it. count(*) decodes nothing.
 *
 * The decoded by-reference values are allocated in the slot's memory context and freed when the slot is cleared or overwritten, like the
 * tuple of a buffer slot (no memory context of its own: a slot that is never dropped, as in a worker, holds nothing else). A slot
 * whose unit is no longer the current one of its group scan (another row was fetched meanwhile) finds its row again by its TID.
 */
#include "postgres.h"

#include "access/htup_details.h"
#include "access/sysattr.h"
#include "executor/tuptable.h"
#include "utils/memutils.h"
#include "utils/rel.h"

#include "sift_table.h"

typedef struct SiftTblSlot
{
	VirtualTupleTableSlot base;
	void	  **owned;			/* the by-reference values decoded since the slot was filled */
	int			nowned;
	int			capowned;
	Relation	rel;
	SiftTblGroupScan *g;		/* the rows of a column group are decoded from g (referenced) while its token is the slot's; NULL: nothing pending */
	uint32		token;
	uint32		pos;
} SiftTblSlot;

static TupleTableSlotOps sift_slot_ops;

const TupleTableSlotOps *
sift_tbl_slot_ops(void)
{
	return &sift_slot_ops;
}

static void
lazy_drop(SiftTblSlot *ss)
{
	if (ss->g)
		sift_tbl_gscan_unref(ss->g);
	ss->g = NULL;
}

static void
slot_release(TupleTableSlot *slot)
{
	SiftTblSlot *ss = (SiftTblSlot *) slot;

	lazy_drop(ss);
	if (ss->owned)
		pfree(ss->owned);
}

static void
slot_clear(TupleTableSlot *slot)
{
	SiftTblSlot *ss = (SiftTblSlot *) slot;

	lazy_drop(ss);
	while (ss->nowned > 0)
		pfree(ss->owned[--ss->nowned]);
	TTSOpsVirtual.clear(slot);
}

/* The row's unit is not the group scan's current one any more: the row is found again by its TID */
static void
relocate(SiftTblSlot *ss, TupleTableSlot *slot)
{
	SiftTblGroupScan *g;

	if (!sift_tbl_group_locate(ss->rel, sift_tbl_rel(ss->rel), &slot->tts_tid, &g, &ss->token, &ss->pos))
		elog(ERROR, "sift_table: the row (%u,%u) of \"%s\" is gone", ItemPointerGetBlockNumberNoCheck(&slot->tts_tid),
			 ItemPointerGetOffsetNumberNoCheck(&slot->tts_tid), RelationGetRelationName(ss->rel));
	sift_tbl_gscan_ref(g);
	lazy_drop(ss);
	ss->g = g;
}

static void
own(SiftTblSlot *ss, TupleTableSlot *slot, void *p)
{
	if (ss->nowned == ss->capowned)
	{
		ss->capowned = Max(8, ss->capowned * 2);
		ss->owned = ss->owned ? repalloc(ss->owned, ss->capowned * sizeof(void *)) : MemoryContextAlloc(slot->tts_mcxt, ss->capowned * sizeof(void *));
	}
	ss->owned[ss->nowned++] = p;
}

static void
own_values(SiftTblSlot *ss, TupleTableSlot *slot, int from, int to)
{
	for (int a = from; a < to; a++)
		if (!slot->tts_isnull[a] && !TupleDescAttr(slot->tts_tupleDescriptor, a)->attbyval)
			own(ss, slot, DatumGetPointer(slot->tts_values[a]));
}

static void
sift_slot_getsomeattrs(TupleTableSlot *slot, int natts)
{
	SiftTblSlot *ss = (SiftTblSlot *) slot;
	MemoryContext old = MemoryContextSwitchTo(slot->tts_mcxt);

	natts = Min(natts, slot->tts_tupleDescriptor->natts);
	if (!sift_tbl_gscan_fill(ss->g, ss->token, ss->pos, slot->tts_nvalid, natts, slot->tts_values, slot->tts_isnull))
	{
		relocate(ss, slot);
		if (!sift_tbl_gscan_fill(ss->g, ss->token, ss->pos, slot->tts_nvalid, natts, slot->tts_values, slot->tts_isnull))
			elog(ERROR, "sift_table: the unit of row (%u,%u) changed while it was read", ItemPointerGetBlockNumberNoCheck(&slot->tts_tid),
				 ItemPointerGetOffsetNumberNoCheck(&slot->tts_tid));
	}
	own_values(ss, slot, slot->tts_nvalid, natts);
	slot->tts_nvalid = natts;
	MemoryContextSwitchTo(old);
}

/* The transaction columns read as a tuple made by heap_form_tuple does: zero (the row's visibility is the table's, not the slot's) */
static Datum
sift_slot_getsysattr(TupleTableSlot *slot, int attnum, bool *isnull)
{
	if (attnum > MinTransactionIdAttributeNumber || attnum < MaxCommandIdAttributeNumber)
		return TTSOpsVirtual.getsysattr(slot, attnum, isnull);
	*isnull = false;
	return (Datum) 0;
}

/* Decodes what is left: the callbacks below read tts_values of every attribute */
static void
realize(TupleTableSlot *slot)
{
	SiftTblSlot *ss = (SiftTblSlot *) slot;

	if (ss->g == NULL)
		return;
	sift_slot_getsomeattrs(slot, slot->tts_tupleDescriptor->natts);
	lazy_drop(ss);
}

static void
slot_materialize(TupleTableSlot *slot)
{
	realize(slot);
	TTSOpsVirtual.materialize(slot);
}

static HeapTuple
slot_copy_heap_tuple(TupleTableSlot *slot)
{
	realize(slot);
	return TTSOpsVirtual.copy_heap_tuple(slot);
}

#if PG_VERSION_NUM >= 180000
static MinimalTuple
slot_copy_minimal_tuple(TupleTableSlot *slot, Size extra)
{
	realize(slot);
	return TTSOpsVirtual.copy_minimal_tuple(slot, extra);
}
#else
static MinimalTuple
slot_copy_minimal_tuple(TupleTableSlot *slot)
{
	realize(slot);
	return TTSOpsVirtual.copy_minimal_tuple(slot);
}
#endif

void
sift_tbl_slot_init(void)
{
	sift_slot_ops = TTSOpsVirtual;
	sift_slot_ops.base_slot_size = sizeof(SiftTblSlot);
	sift_slot_ops.release = slot_release;
	sift_slot_ops.clear = slot_clear;
	sift_slot_ops.getsomeattrs = sift_slot_getsomeattrs;
	sift_slot_ops.getsysattr = sift_slot_getsysattr;
	sift_slot_ops.materialize = slot_materialize;
	sift_slot_ops.copy_heap_tuple = slot_copy_heap_tuple;
	sift_slot_ops.copy_minimal_tuple = slot_copy_minimal_tuple;
}

void
sift_tbl_slot_store_group(TupleTableSlot *slot, Relation rel, ItemPointer tid, SiftTblGroupScan *g, uint32 token, uint32 pos)
{
	SiftTblSlot *ss = (SiftTblSlot *) slot;

	Assert(slot->tts_ops == &sift_slot_ops);
	ExecClearTuple(slot);
	sift_tbl_gscan_ref(g);
	ss->g = g;
	ss->rel = rel;
	ss->token = token;
	ss->pos = pos;
	slot->tts_tid = *tid;
	slot->tts_tableOid = RelationGetRelid(rel);
	slot->tts_flags &= ~TTS_FLAG_EMPTY;
	slot->tts_nvalid = 0;
}

bool
sift_tbl_slot_locate(TupleTableSlot *slot, Relation rel, SiftTblRel *st, ItemPointer tid)
{
	SiftTblGroupScan *g;
	uint32		token,
				pos;

	if (!sift_tbl_group_locate(rel, st, tid, &g, &token, &pos))
		return false;
	sift_tbl_slot_store_group(slot, rel, tid, g, token, pos);
	return true;
}

/* The slot keeps a copy of tup (the caller frees its own): the values point into it, nothing else is copied */
void
sift_tbl_slot_store_tuple(TupleTableSlot *slot, HeapTuple tup)
{
	SiftTblSlot *ss = (SiftTblSlot *) slot;
	MemoryContext old;
	HeapTuple	copy;

	Assert(slot->tts_ops == &sift_slot_ops);
	ExecClearTuple(slot);
	old = MemoryContextSwitchTo(slot->tts_mcxt);
	copy = heap_copytuple(tup);
	MemoryContextSwitchTo(old);
	heap_deform_tuple(copy, slot->tts_tupleDescriptor, slot->tts_values, slot->tts_isnull);
	own(ss, slot, copy);
	ExecStoreVirtualTuple(slot);
	slot->tts_tid = tup->t_self;
	slot->tts_tableOid = tup->t_tableOid;
}
