/*
 * table/f2/f2_slot.c - the lazy slot of a sift_table of format major 2 (f2_slot.h), on the model of the virtual slot: its callbacks are those of it, except the ones that read
 * the attributes, which decode what is still to be read first.
 */
#include "postgres.h"

#include "access/htup_details.h"
#include "catalog/pg_type.h"
#include "utils/memutils.h"
#include "utils/rel.h"

#include "col/f2/f2_store_schema.h"
#include "table/f2/f2_gcache.h"
#include "table/f2/f2_slot.h"
#include "table/f2/f2_tbl.h"

/* The part map a slot reads the patches of its rows from, kept across the rows of a partition: given back when the slot goes or its memory does (the holder lives in the slot's own context, which is what ends on an error) */
typedef struct PartMapHolder
{
	MemoryContextCallback cb;
	SiftColDeltaPartMap *map;
} PartMapHolder;

typedef struct SiftF2Slot
{
	VirtualTupleTableSlot base;
	Relation	rel;
	SiftF2GranuleBuf *gb;		/* referenced while attributes are still to be decoded from it */
	uint16		row;
	const SiftColDeltaGranMap *gm;	/* a granule map of the part map the slot holds */
	PartMapHolder *holder;
	MemoryContext rowcx;		/* the values by reference: reset when the slot is cleared */
} SiftF2Slot;

static TupleTableSlotOps f2_slot_ops;
static bool f2_slot_ops_ready = false;

static inline int
align_of(Form_pg_attribute att)
{
	return att->attalign == TYPALIGN_DOUBLE ? 8 : att->attalign == TYPALIGN_INT ? 4 : att->attalign == TYPALIGN_SHORT ? 2 : 1;
}

/* The datum of an integer cell: a scaled integer is a numeric of the scale the part wrote it with */
static Datum
int_datum(Form_pg_attribute att, int64 v, uint8 scale)
{
	if (att->atttypid == NUMERICOID)
		return sift_f2_num64_datum(v, scale);
	return att->attlen == 8 ? Int64GetDatum(v) : att->attlen == 4 ? Int32GetDatum((int32) v) : att->attlen == 2 ? Int16GetDatum((int16) v) : CharGetDatum((char) v);
}

static Datum
patch_datum(Form_pg_attribute att, SiftCellKind kind, uint8 scale, const SiftColDeltaPatch *p, MemoryContext cx)
{
	if (kind == SIFT_CELL_INT)
		return int_datum(att, p->val.i, scale);
	if (kind == SIFT_CELL_F8)
		return att->atttypid == FLOAT4OID ? Float4GetDatum((float4) p->val.f) : Float8GetDatum(p->val.f);
	return PointerGetDatum(memcpy(MemoryContextAlloc(cx, p->vlen), p->val.s, p->vlen));
}

static bool
is_wide_blob(const SiftF2Cell *c)
{
	return c->tag == SIFT_COLTAG_WIDE_BLOB;
}

/* A string of the granule is copied when the granule may go before the row does (the point cache's) or when its bytes are not aligned for the type */
static Datum
str_datum(Relation rel, const SiftF2Store *store, const SiftF2GranuleBuf *gb, int phys, uint16 row, const SiftF2Cell *c, Form_pg_attribute att, MemoryContext cx)
{
	if (is_wide_blob(c))
	{
		uint32		len;
		const char *ref = sift_granule_exc_payload(&gb->view, phys, row, &len);

		return store->host->read_wide_blob(rel, ref, len, att->atttypid, att->attlen, cx);
	}
	if (gb->lazy == NULL && ((uintptr_t) c->s.p % align_of(att)) == 0)
		return PointerGetDatum(c->s.p);
	return PointerGetDatum(memcpy(MemoryContextAlloc(cx, c->s.len), c->s.p, c->s.len));
}

/* True when the attribute made the granule decode a column that no row asked for before: only then does the cache hold more */
static bool
fill_att(Relation rel, const SiftF2Store *store, SiftF2GranuleBuf *gb, uint16 row, const SiftColDeltaGranMap *gm, TupleTableSlot *slot, int a, MemoryContext cx)
{
	Form_pg_attribute att = TupleDescAttr(slot->tts_tupleDescriptor, a - 1);
	const SiftF2ColMap *map = gb->pin->col_map;
	const SiftColDeltaPatch *p;
	SiftF2Cell	cell;
	SiftCellKind kind;
	int			leaf,
				phys;
	bool		decoded;

	slot->tts_isnull[a - 1] = true;
	slot->tts_values[a - 1] = (Datum) 0;
	if (a > store->natts || (leaf = store->att_first[a]) < 0)
		return false;
	phys = map != NULL ? map->phys_col[leaf] : leaf;
	gm = gm != NULL || phys >= 0 ? gm : gb->patch_arg;
	p = gm != NULL ? sift_col_delta_find_row(gm, (int16) a, row) : NULL;
	if (p != NULL)
	{
		slot->tts_isnull[a - 1] = (p->flags & SIFT_CDCOL_IS_NULL) != 0;
		slot->tts_values[a - 1] = slot->tts_isnull[a - 1] ? (Datum) 0 : patch_datum(att, store->kinds[leaf], store->scales[leaf], p, cx);
		return false;
	}
	if (phys < 0)
	{
		slot->tts_isnull[a - 1] = map->missing_null[leaf];
		slot->tts_values[a - 1] = map->missing_val[leaf];
		return false;
	}
	kind = map == NULL ? store->kinds[leaf] : (SiftCellKind) map->phys_kind[leaf];
	decoded = sift_f2_gbuf_cell(rel, gb, phys, kind, row, sift_tbl_extra(store)->has_wide || sift_f2_attr_may_have_exceptions(att), &cell);
	if (cell.isnull && !is_wide_blob(&cell) && !(kind == SIFT_CELL_INT && cell.tag == SIFT_COLTAG_UNSURE))
		return decoded;
	slot->tts_isnull[a - 1] = false;
	if (kind == SIFT_CELL_INT && cell.isnull)
	{
		uint32		len;
		const char *payload = sift_granule_exc_payload(&gb->view, phys, row, &len);

		slot->tts_values[a - 1] = sift_f2_num64_exception_datum(payload, len, cx);
	}
	else if (kind == SIFT_CELL_INT)
		slot->tts_values[a - 1] = int_datum(att, cell.i, map != NULL ? map->old_scale[leaf] : store->scales[leaf]);
	else if (kind == SIFT_CELL_F8)
		slot->tts_values[a - 1] = att->atttypid == FLOAT4OID ? Float4GetDatum((float4) cell.f) : Float8GetDatum(cell.f);
	else
		slot->tts_values[a - 1] = str_datum(rel, store, gb, phys, row, &cell, att, cx);
	return decoded;
}

static void
holder_release(void *arg)
{
	PartMapHolder *h = arg;

	sift_col_delta_part_map_unref(h->map);
	h->map = NULL;
}

static void
hold_part_map(SiftF2Slot *ss, SiftColDeltaPartMap *map)
{
	if (ss->holder == NULL)
	{
		ss->holder = MemoryContextAllocZero(ss->base.base.tts_mcxt, sizeof(PartMapHolder));
		ss->holder->cb.func = holder_release;
		ss->holder->cb.arg = ss->holder;
		MemoryContextRegisterResetCallback(ss->base.base.tts_mcxt, &ss->holder->cb);
	}
	if (ss->holder->map == map)
		return;
	sift_col_delta_part_map_unref(ss->holder->map);
	ss->holder->map = sift_col_delta_part_map_ref(map);
}

static void
lazy_drop(SiftF2Slot *ss)
{
	if (ss->gb != NULL)
		sift_f2_gbuf_unref(ss->gb);
	ss->gb = NULL;
}

static void
slot_init(TupleTableSlot *slot)
{
	/* MEM[MEM-TBL-14]: slot-scoped f2 slot rowcx (ALLOCSET_SMALL_SIZES; reset per tuple) */
	((SiftF2Slot *) slot)->rowcx = AllocSetContextCreate(slot->tts_mcxt, "sift f2 slot row", ALLOCSET_SMALL_SIZES);
}

static void
slot_release(TupleTableSlot *slot)
{
	SiftF2Slot *ss = (SiftF2Slot *) slot;

	lazy_drop(ss);
	if (ss->holder != NULL)
		holder_release(ss->holder);
	MemoryContextDelete(ss->rowcx);
}

static void
slot_clear(TupleTableSlot *slot)
{
	SiftF2Slot *ss = (SiftF2Slot *) slot;

	lazy_drop(ss);
	ss->gm = NULL;
	MemoryContextReset(ss->rowcx);
	TTSOpsVirtual.clear(slot);
}

static void
f2_slot_getsomeattrs(TupleTableSlot *slot, int natts)
{
	SiftF2Slot *ss = (SiftF2Slot *) slot;
	const SiftF2Store *store = sift_f2_store_open(ss->rel);
	MemoryContext old = MemoryContextSwitchTo(ss->rowcx);
	bool		decoded = false;

	natts = Min(natts, slot->tts_tupleDescriptor->natts);
	for (int a = slot->tts_nvalid + 1; a <= natts; a++)
	{
		decoded |= fill_att(ss->rel, store, ss->gb, ss->row, ss->gm, slot, a, ss->rowcx);
	}
	MemoryContextSwitchTo(old);
	slot->tts_nvalid = natts;
	if (decoded && ss->gb->cached)
		sift_f2_gcache_charge();
}

/* Decodes what is left: the callbacks below read tts_values of every attribute */
static void
realize(TupleTableSlot *slot)
{
	SiftF2Slot *ss = (SiftF2Slot *) slot;

	if (ss->gb == NULL)
		return;
	f2_slot_getsomeattrs(slot, slot->tts_tupleDescriptor->natts);
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
	HeapTuple	tuple;

	realize(slot);
	tuple = TTSOpsVirtual.copy_heap_tuple(slot);
	tuple->t_self = slot->tts_tid;
	return tuple;
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

const TupleTableSlotOps *
sift_f2_slot_ops(void)
{
	if (!f2_slot_ops_ready)
	{
		f2_slot_ops = TTSOpsVirtual;
		f2_slot_ops.base_slot_size = sizeof(SiftF2Slot);
		f2_slot_ops.init = slot_init;
		f2_slot_ops.release = slot_release;
		f2_slot_ops.clear = slot_clear;
		f2_slot_ops.getsomeattrs = f2_slot_getsomeattrs;
		f2_slot_ops.materialize = slot_materialize;
		f2_slot_ops.copy_heap_tuple = slot_copy_heap_tuple;
		f2_slot_ops.copy_minimal_tuple = slot_copy_minimal_tuple;
		f2_slot_ops_ready = true;
	}
	return &f2_slot_ops;
}

void
sift_f2_slot_store(TupleTableSlot *slot, Relation rel, SiftF2GranuleBuf *gb, uint16 row, SiftColDeltaPartMap *map, const SiftColDeltaGranMap *gm, MemoryContext rowcx)
{
	uint64		tid = sift_f2_gbuf_tids(gb)[row];

	ExecClearTuple(slot);
	if (slot->tts_ops != &f2_slot_ops)
	{
		const SiftF2Store *store = sift_f2_store_open(rel);

		for (int a = 1; a <= slot->tts_tupleDescriptor->natts; a++)
			(void) fill_att(rel, store, gb, row, gm, slot, a, rowcx);
		ExecStoreVirtualTuple(slot);
	}
	else
	{
		SiftF2Slot *ss = (SiftF2Slot *) slot;

		ss->rel = rel;
		ss->gb = sift_f2_gbuf_ref(gb);
		ss->row = row;
		ss->gm = gm;
		if (gm != NULL)
			hold_part_map(ss, map);
		slot->tts_flags &= ~TTS_FLAG_EMPTY;
		slot->tts_nvalid = 0;
	}
	ItemPointerSet(&slot->tts_tid, (BlockNumber) (tid >> 16), (OffsetNumber) (tid & 0xFFFF));
}
