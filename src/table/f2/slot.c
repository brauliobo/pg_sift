/*
 * table/f2/slot.c - the lazy slot of a sift_table of format major 2 (f2_slot.h), on the model of the virtual slot: its callbacks are those of it, except the ones that read
 * the attributes, which decode what is still to be read first.
 */
#include "postgres.h"

#include "access/htup_details.h"
#include "access/sysattr.h"
#include "catalog/pg_type.h"
#include "utils/memutils.h"
#include "utils/rel.h"

#include "col/f2/store/schema.h"
#include "table/f2/gcache.h"
#include "table/f2/row/pack.h"
#include "table/f2/row/state.h"
#include "table/f2/slot.h"
#include "table/f2/tbl.h"
#include "core/host.h"

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
	const SiftRowPackRow *rp;	/* a row of the row pack of the part (in rowcx): the cells are read from it, and from the granule only when it is external */
	SiftF2PartPin *pin;			/* referenced while rp is: the part the row is in */
	uint32		granule_id;
	bool		decoded_any;	/* decoded_ahead has a bit set */
	uint64		decoded_ahead[(MaxTupleAttributeNumber + 63) / 64];	/* the attributes a read of one attribute decoded ahead of the leading ones */
} SiftF2Slot;

/* Where the cells of a row are read from: its granule, or its row of a row pack with the granule it needs for an external cell, read when one is asked for */
typedef struct CellSource
{
	SiftF2GranuleBuf **gb;
	SiftF2PartPin *pin;
	uint32		granule_id;
	uint16		row;			/* in the granule */
	const SiftRowPackRow *rp;
} CellSource;

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

/* A string the table keeps as a datum image (any type but those a headerless store keeps bare) is copied: the granule may go before the row does, and the bytes may not be aligned for the type */
static Datum
str_image_datum(const char *bytes, uint32 len, MemoryContext cx)
{
	return PointerGetDatum(memcpy(MemoryContextAlloc(cx, len), bytes, len));
}

static Datum
patch_datum(const SiftF2Store *store, Form_pg_attribute att, SiftCellKind kind, uint8 scale, const SiftColDeltaPatch *p, MemoryContext cx)
{
	if (kind == SIFT_CELL_INT)
		return int_datum(att, p->val.i, scale);
	if (kind == SIFT_CELL_F8)
		return att->atttypid == FLOAT4OID ? Float4GetDatum((float4) p->val.f) : Float8GetDatum(p->val.f);
	return store->headerless && sift_f2_attr_bare_bytes(att) ? sift_f2_bare_datum(att, p->val.s, p->vlen, cx) : str_image_datum(p->val.s, p->vlen, cx);
}

static bool
is_wide_blob(const SiftF2Cell *c)
{
	return c->tag == SIFT_COLTAG_WIDE_BLOB;
}

/* A string of the granule is copied when the granule may go before the row does (the point cache's) or when its bytes are not aligned for the type; one of a row pack goes with the row */
static Datum
str_datum(Relation rel, const SiftF2Store *store, const SiftF2GranuleBuf *gb, int phys, uint16 row, const SiftF2Cell *c, Form_pg_attribute att, bool stable, MemoryContext cx)
{
	if (is_wide_blob(c))
	{
		uint32		len;
		const char *ref = sift_granule_exc_payload(&gb->view, phys, row, &len);

		return store->host->read_wide_blob(rel, ref, len, att->atttypid, att->attlen, cx);
	}
	if (store->headerless && sift_f2_attr_bare_bytes(att))
		return sift_f2_bare_datum(att, c->s.p, c->s.len, cx);
	if (stable && ((uintptr_t) c->s.p % align_of(att)) == 0)
		return PointerGetDatum(c->s.p);
	return str_image_datum(c->s.p, c->s.len, cx);
}

/* The string or the double of a lazy granule's framed chunk, from the frame the cache keeps; false when the chunk is not framed or the cell is null */
static bool
frame_cell(Relation rel, const SiftF2GranuleBuf *gb, int phys, SiftCellKind kind, uint16 row, SiftF2Cell *cell)
{
	const int	chunk = phys + 1;

	if (kind == SIFT_CELL_INT || gb->lazy == NULL || gb->col_decoded[phys])
		return false;
	sift_lazy_granule_load(rel, gb->lazy, &chunk, 1);
	*cell = (SiftF2Cell) {0};
	return kind == SIFT_CELL_F8 ? sift_f2_gcache_cell_f8(rel, gb, phys, row, &cell->f) : sift_f2_gcache_cell_str(rel, gb, phys, row, &cell->s);
}

/* The cell of column phys; true when it made the granule decode the column whole */
static bool
read_cell(Relation rel, SiftF2Store *store, const CellSource *src, int phys, SiftCellKind kind, bool wide, SiftF2Cell *cell)
{
	if (src->rp != NULL && sift_row_pack_cell(src->rp, phys, cell) != SIFT_RPACK_EXTERNAL)
		return false;
	if (*src->gb == NULL)
		*src->gb = sift_f2_gcache_get(rel, store, src->pin, src->granule_id);
	if (frame_cell(rel, *src->gb, phys, kind, src->row, cell))
		return false;
	return sift_f2_gbuf_cell(rel, *src->gb, phys, kind, src->row, wide, cell);
}

/* True when the attribute made the granule decode a column that no row asked for before: only then does the cache hold more */
static bool
fill_att(Relation rel, SiftF2Store *store, const CellSource *src, const SiftColDeltaGranMap *gm, TupleTableSlot *slot, int a, MemoryContext cx)
{
	Form_pg_attribute att = TupleDescAttr(slot->tts_tupleDescriptor, a - 1);
	const SiftF2ColMap *map = src->pin->col_map;
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
	gm = gm != NULL || phys >= 0 || *src->gb == NULL ? gm : (*src->gb)->patch_arg;
	p = gm != NULL ? sift_col_delta_find_row(gm, (int16) a, src->row) : NULL;
	if (p != NULL)
	{
		slot->tts_isnull[a - 1] = (p->flags & SIFT_CDCOL_IS_NULL) != 0;
		slot->tts_values[a - 1] = slot->tts_isnull[a - 1] ? (Datum) 0 : patch_datum(store, att, store->kinds[leaf], store->scales[leaf], p, cx);
		return false;
	}
	if (phys < 0)
	{
		slot->tts_isnull[a - 1] = map->missing_null[leaf];
		slot->tts_values[a - 1] = map->missing_val[leaf];
		return false;
	}
	kind = map == NULL ? store->kinds[leaf] : (SiftCellKind) map->phys_kind[leaf];
	decoded = read_cell(rel, store, src, phys, kind, sift_tbl_extra(store)->has_wide || sift_f2_attr_may_have_exceptions(att), &cell);
	if (cell.isnull && !is_wide_blob(&cell) && !(kind == SIFT_CELL_INT && cell.tag == SIFT_COLTAG_UNSURE))
		return decoded;
	slot->tts_isnull[a - 1] = false;
	if (kind == SIFT_CELL_INT && cell.isnull)
	{
		uint32		len;
		const char *payload = sift_granule_exc_payload(&(*src->gb)->view, phys, src->row, &len);

		slot->tts_values[a - 1] = sift_f2_num64_exception_datum(payload, len, cx);
	}
	else if (kind == SIFT_CELL_INT)
		slot->tts_values[a - 1] = int_datum(att, cell.i, map != NULL ? map->old_scale[leaf] : store->scales[leaf]);
	else if (kind == SIFT_CELL_F8)
		slot->tts_values[a - 1] = att->atttypid == FLOAT4OID ? Float4GetDatum((float4) cell.f) : Float8GetDatum(cell.f);
	else
		slot->tts_values[a - 1] = str_datum(rel, store, *src->gb, phys, src->row, &cell, att, *src->gb == NULL || (*src->gb)->lazy == NULL, cx);
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
	if (ss->pin != NULL)
		sift_f2_pin_release(ss->pin);
	ss->gb = NULL;
	ss->pin = NULL;
	ss->rp = NULL;
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
	if (ss->decoded_any)
		memset(ss->decoded_ahead, 0, sizeof(ss->decoded_ahead));
	ss->decoded_any = false;
	MemoryContextReset(ss->rowcx);
	TTSOpsVirtual.clear(slot);
}

static CellSource
slot_source(SiftF2Slot *ss)
{
	return (CellSource) {&ss->gb, ss->pin != NULL ? ss->pin : ss->gb->pin, ss->granule_id, ss->row, ss->rp};
}

static inline bool
is_decoded_ahead(const SiftF2Slot *ss, int a)
{
	return (ss->decoded_ahead[(a - 1) / 64] & (UINT64CONST(1) << ((a - 1) % 64))) != 0;
}

static void
f2_slot_getsomeattrs(TupleTableSlot *slot, int natts)
{
	SiftF2Slot *ss = (SiftF2Slot *) slot;
	SiftF2Store *store = sift_f2_store_open(ss->rel);
	const CellSource src = slot_source(ss);
	MemoryContext old = MemoryContextSwitchTo(ss->rowcx);
	bool		decoded = false;

	natts = Min(natts, slot->tts_tupleDescriptor->natts);
	for (int a = slot->tts_nvalid + 1; a <= natts; a++)
		if (!is_decoded_ahead(ss, a))
			decoded |= fill_att(ss->rel, store, &src, ss->gm, slot, a, ss->rowcx);
	MemoryContextSwitchTo(old);
	slot->tts_nvalid = natts;
	if (decoded && ss->gb != NULL && ss->gb->cached)
		sift_f2_gcache_charge();
}

Datum
sift_f2_slot_getattr_single(TupleTableSlot *slot, int attnum, bool *isnull)
{
	SiftF2Slot *ss = (SiftF2Slot *) slot;

	if (attnum > slot->tts_nvalid && !is_decoded_ahead(ss, attnum))
	{
		const CellSource src = slot_source(ss);

		(void) fill_att(ss->rel, sift_f2_store_open(ss->rel), &src, ss->gm, slot, attnum, ss->rowcx);
		ss->decoded_ahead[(attnum - 1) / 64] |= UINT64CONST(1) << ((attnum - 1) % 64);
		ss->decoded_any = true;
	}
	*isnull = slot->tts_isnull[attnum - 1];
	return slot->tts_values[attnum - 1];
}

/* xmin and cmin are the stored versions of the insertion of the row (FrozenTransactionId and command 0 once frozen), xmax and cmax those of its last delete, update or lock (0 when none) */
static Datum
f2_slot_getsysattr(TupleTableSlot *slot, int attnum, bool *isnull)
{
	SiftF2Slot *ss = (SiftF2Slot *) slot;
	SiftRowState rs;

	if (attnum != MinTransactionIdAttributeNumber && attnum != MinCommandIdAttributeNumber && attnum != MaxTransactionIdAttributeNumber && attnum != MaxCommandIdAttributeNumber)
		return TTSOpsVirtual.getsysattr(slot, attnum, isnull);
	*isnull = false;
	if (!ItemPointerIsValid(&slot->tts_tid))
		return (Datum) 0;
	rs = sift_row_state_read(ss->rel, sift_tbl_tid_pack(&slot->tts_tid));
	SIFT_INVARIANT(rs.found, "the row (%u,%u) of \"%s\" is gone", ItemPointerGetBlockNumberNoCheck(&slot->tts_tid), ItemPointerGetOffsetNumberNoCheck(&slot->tts_tid),
				   RelationGetRelationName(ss->rel));
	switch (attnum)
	{
		case MinTransactionIdAttributeNumber:
			return TransactionIdGetDatum(TransactionIdIsValid(rs.xmin) ? rs.xmin : FrozenTransactionId);
		case MinCommandIdAttributeNumber:
			return CommandIdGetDatum(rs.cmin);
		case MaxTransactionIdAttributeNumber:
			return TransactionIdGetDatum(rs.xmax);
		default:
			return CommandIdGetDatum(rs.cmax);
	}
}

/* Decodes what is left: the callbacks below read tts_values of every attribute */
static void
realize(TupleTableSlot *slot)
{
	SiftF2Slot *ss = (SiftF2Slot *) slot;

	if (ss->gb == NULL && ss->rp == NULL)
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
		f2_slot_ops.getsysattr = f2_slot_getsysattr;
		f2_slot_ops.materialize = slot_materialize;
		f2_slot_ops.copy_heap_tuple = slot_copy_heap_tuple;
		f2_slot_ops.copy_minimal_tuple = slot_copy_minimal_tuple;
		f2_slot_ops_ready = true;
	}
	return &f2_slot_ops;
}

static const SiftRowPackRow *
pack_row(const SiftRowPackBlk *blk, uint32 blk_row, MemoryContext cx)
{
	MemoryContext old = MemoryContextSwitchTo(cx);
	const SiftRowPackRow *rp = sift_row_pack_row(blk, blk_row);

	MemoryContextSwitchTo(old);
	return rp;
}

static void
store_source(TupleTableSlot *slot, Relation rel, SiftF2PartPin *pin, uint32 granule_id, uint16 row, const SiftRowPackBlk *blk, uint32 blk_row, SiftF2GranuleBuf *gb, uint64 tid,
			 SiftColDeltaPartMap *map, const SiftColDeltaGranMap *gm, MemoryContext rowcx)
{
	ExecClearTuple(slot);
	if (slot->tts_ops != &f2_slot_ops)
	{
		SiftF2Store *store = sift_f2_store_open(rel);
		SiftF2GranuleBuf *loaded = gb;
		const CellSource src = {&loaded, pin, granule_id, row, blk != NULL ? pack_row(blk, blk_row, rowcx) : NULL};

		for (int a = 1; a <= slot->tts_tupleDescriptor->natts; a++)
			(void) fill_att(rel, store, &src, gm, slot, a, rowcx);
		ExecStoreVirtualTuple(slot);
		if (loaded != gb)
			sift_f2_gbuf_unref(loaded);
	}
	else
	{
		SiftF2Slot *ss = (SiftF2Slot *) slot;

		ss->rel = rel;
		ss->gb = gb != NULL ? sift_f2_gbuf_ref(gb) : NULL;
		ss->rp = blk != NULL ? pack_row(blk, blk_row, ss->rowcx) : NULL;
		if (blk != NULL)
			pin->refcnt++;
		ss->pin = blk != NULL ? pin : NULL;
		ss->granule_id = granule_id;
		ss->row = row;
		ss->gm = gm;
		if (gm != NULL)
			hold_part_map(ss, map);
		slot->tts_flags &= ~TTS_FLAG_EMPTY;
		slot->tts_nvalid = 0;
	}
	ItemPointerSet(&slot->tts_tid, (BlockNumber) (tid >> 16), (OffsetNumber) (tid & 0xFFFF));
}

void
sift_f2_slot_store(TupleTableSlot *slot, Relation rel, SiftF2GranuleBuf *gb, uint16 row, SiftColDeltaPartMap *map, const SiftColDeltaGranMap *gm, MemoryContext rowcx)
{
	store_source(slot, rel, gb->pin, gb->granule_id, row, NULL, 0, gb, sift_f2_gbuf_tids(gb)[row], map, gm, rowcx);
}

void
sift_f2_slot_store_packed(TupleTableSlot *slot, Relation rel, SiftF2PartPin *pin, uint32 granule_id, uint16 row, const SiftRowPackBlk *blk, uint32 blk_row, uint64 tid, SiftF2GranuleBuf *gb,
						  SiftColDeltaPartMap *map, const SiftColDeltaGranMap *gm, MemoryContext rowcx)
{
	store_source(slot, rel, pin, granule_id, row, blk, blk_row, gb, tid, map, gm, rowcx);
}
