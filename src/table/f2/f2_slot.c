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

typedef struct SiftF2Slot
{
	VirtualTupleTableSlot base;
	Relation	rel;
	SiftF2GranuleBuf *gb;		/* referenced while attributes are still to be decoded from it */
	uint16		row;
	const SiftColDeltaGranMap *gm;
	MemoryContext rowcx;		/* the values by reference and the patches of the row: reset when the slot is cleared */
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
is_wide_blob(const SiftF2GranuleBuf *gb, int phys, uint16 row)
{
	return gb->col_tag[phys] != NULL && gb->col_tag[phys][row] == SIFT_COLTAG_WIDE_BLOB;
}

/* A string of the granule is copied when the granule may go before the row does (the point cache's) or when its bytes are not aligned for the type */
static Datum
str_datum(Relation rel, const SiftF2Store *store, const SiftF2GranuleBuf *gb, int phys, uint16 row, Form_pg_attribute att, MemoryContext cx)
{
	const SiftStrCell *c = &gb->col_s[phys][row];

	if (is_wide_blob(gb, phys, row))
	{
		uint32		len;
		const char *ref = sift_granule_exc_payload(&gb->view, phys, row, &len);

		return store->host->read_wide_blob(rel, ref, len, att->atttypid, att->attlen, cx);
	}
	if (gb->lazy == NULL && ((uintptr_t) c->p % align_of(att)) == 0)
		return PointerGetDatum(c->p);
	return PointerGetDatum(memcpy(MemoryContextAlloc(cx, c->len), c->p, c->len));
}

static void
fill_att(Relation rel, const SiftF2Store *store, SiftF2GranuleBuf *gb, uint16 row, const SiftColDeltaGranMap *gm, TupleTableSlot *slot, int a, MemoryContext cx)
{
	Form_pg_attribute att = TupleDescAttr(slot->tts_tupleDescriptor, a - 1);
	const SiftF2ColMap *map = gb->pin->col_map;
	const SiftColDeltaPatch *p;
	SiftCellKind kind;
	int			leaf,
				phys;

	slot->tts_isnull[a - 1] = true;
	slot->tts_values[a - 1] = (Datum) 0;
	if (a > store->natts || (leaf = store->att_first[a]) < 0)
		return;
	phys = map != NULL ? map->phys_col[leaf] : leaf;
	gm = gm != NULL || phys >= 0 ? gm : gb->patch_arg;
	p = gm != NULL ? sift_col_delta_find_row(gm, (int16) a, row) : NULL;
	if (p != NULL)
	{
		slot->tts_isnull[a - 1] = (p->flags & SIFT_CDCOL_IS_NULL) != 0;
		slot->tts_values[a - 1] = slot->tts_isnull[a - 1] ? (Datum) 0 : patch_datum(att, store->kinds[leaf], store->scales[leaf], p, cx);
		return;
	}
	if (phys < 0)
	{
		slot->tts_isnull[a - 1] = map->missing_null[leaf];
		slot->tts_values[a - 1] = map->missing_val[leaf];
		return;
	}
	kind = map == NULL ? store->kinds[leaf] : (SiftCellKind) map->phys_kind[leaf];
	if (!gb->col_decoded[phys])
		sift_f2_gbuf_column(rel, gb, phys, kind, sift_tbl_extra(store)->has_wide || sift_f2_attr_may_have_exceptions(att));
	if (gb->col_isnull[phys][row] && !is_wide_blob(gb, phys, row) && !(kind == SIFT_CELL_INT && gb->col_tag[phys] != NULL && gb->col_tag[phys][row] == SIFT_COLTAG_UNSURE))
		return;
	slot->tts_isnull[a - 1] = false;
	if (kind == SIFT_CELL_INT && gb->col_isnull[phys][row])
	{
		uint32		len;
		const char *payload = sift_granule_exc_payload(&gb->view, phys, row, &len);

		slot->tts_values[a - 1] = sift_f2_num64_exception_datum(payload, len, cx);
	}
	else if (kind == SIFT_CELL_INT)
		slot->tts_values[a - 1] = int_datum(att, gb->col_i[phys][row], map != NULL ? map->old_scale[leaf] : store->scales[leaf]);
	else if (kind == SIFT_CELL_F8)
		slot->tts_values[a - 1] = att->atttypid == FLOAT4OID ? Float4GetDatum((float4) gb->col_f[phys][row]) : Float8GetDatum(gb->col_f[phys][row]);
	else
		slot->tts_values[a - 1] = str_datum(rel, store, gb, phys, row, att, cx);
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
	((SiftF2Slot *) slot)->rowcx = AllocSetContextCreate(slot->tts_mcxt, "sift f2 slot row", ALLOCSET_SMALL_SIZES);
}

static void
slot_release(TupleTableSlot *slot)
{
	SiftF2Slot *ss = (SiftF2Slot *) slot;

	lazy_drop(ss);
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
		fill_att(ss->rel, store, ss->gb, ss->row, ss->gm, slot, a, ss->rowcx);
		decoded = true;
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
sift_f2_slot_store(TupleTableSlot *slot, Relation rel, SiftF2GranuleBuf *gb, uint16 row, const SiftColDeltaGranMap *gm, MemoryContext rowcx)
{
	uint64		tid = sift_f2_gbuf_tids(gb)[row];

	ExecClearTuple(slot);
	if (slot->tts_ops != &f2_slot_ops)
	{
		const SiftF2Store *store = sift_f2_store_open(rel);

		for (int a = 1; a <= slot->tts_tupleDescriptor->natts; a++)
			fill_att(rel, store, gb, row, gm, slot, a, rowcx);
		ExecStoreVirtualTuple(slot);
	}
	else
	{
		SiftF2Slot *ss = (SiftF2Slot *) slot;

		ss->rel = rel;
		ss->gb = sift_f2_gbuf_ref(gb);
		ss->row = row;
		ss->gm = gm != NULL ? sift_col_delta_gran_copy(gm, ss->rowcx) : NULL;
		slot->tts_flags &= ~TTS_FLAG_EMPTY;
		slot->tts_nvalid = 0;
	}
	ItemPointerSet(&slot->tts_tid, (BlockNumber) (tid >> 16), (OffsetNumber) (tid & 0xFFFF));
}
