/*
 * table/f2/slot.h - the tuple table slot of a sift_table of format major 2: a virtual slot that records which row of which granule it holds and decodes an attribute when an
 * expression asks for it (slot_getsomeattrs reaches it with the number of leading attributes the query reads). A scan or a fetch decodes no column; count(*) reads none, and a
 * lookup that returns one integer reads the blocks of that column, not those of the granule.
 */
#ifndef SIFT_TBL_F2_SLOT_H
#define SIFT_TBL_F2_SLOT_H

#include "executor/tuptable.h"

#include "col/f2/gbuf/gbuf.h"
#include "table/f2/col_delta/col_delta.h"
#include "table/f2/row/pack.h"

extern const TupleTableSlotOps *sift_f2_slot_ops(void);

/*
 * Makes slot hold row of the granule: a reference to gb is kept until the slot is cleared or overwritten. gm: the patches of column deltas the reader's snapshot sees on the granule
 * (a granule map of map, which the slot holds a reference of while it reads them; NULL for none, and for a granule whose decoded columns already hold them: the sequential scan's). A slot of
 * another kind is filled with every attribute now.
 */
extern void sift_f2_slot_store(TupleTableSlot *slot, Relation rel, SiftF2GranuleBuf *gb, uint16 row, SiftColDeltaPartMap *map, const SiftColDeltaGranMap *gm, MemoryContext rowcx);

/*
 * The same for row blk_row of a block of the row pack of a part (copied now, so blk may go after the call): its cells are read from it, and a cell that is not in it from the granule (the slot
 * takes a reference of gb, or reads the granule through the cache when it has none). The slot keeps a reference of pin while it is lazy. row: the row in its granule; tid: its address.
 */
extern void sift_f2_slot_store_packed(TupleTableSlot *slot, Relation rel, SiftF2PartPin *pin, uint32 granule_id, uint16 row, const SiftRowPackBlk *blk, uint32 blk_row, uint64 tid, SiftF2GranuleBuf *gb,
									  SiftColDeltaPartMap *map, const SiftColDeltaGranMap *gm, MemoryContext rowcx);

/* Attribute attnum of the row alone, without the attributes before it: the others are decoded when a read of the leading attributes reaches them (PostgreSQL's callbacks ask for a number of leading ones) */
extern Datum sift_f2_slot_getattr_single(TupleTableSlot *slot, int attnum, bool *isnull);

#endif							/* SIFT_TBL_F2_SLOT_H */
