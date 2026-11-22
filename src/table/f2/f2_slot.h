/*
 * table/f2/f2_slot.h - the tuple table slot of a sift_table of format major 2: a virtual slot that records which row of which granule it holds and decodes an attribute when an
 * expression asks for it (slot_getsomeattrs reaches it with the number of leading attributes the query reads). A scan or a fetch decodes no column; count(*) reads none, and a
 * lookup that returns one integer reads the blocks of that column, not those of the granule.
 */
#ifndef SIFT_TBL_F2_SLOT_H
#define SIFT_TBL_F2_SLOT_H

#include "executor/tuptable.h"

#include "col/f2/f2_gbuf.h"
#include "table/f2/f2_col_delta.h"

extern const TupleTableSlotOps *sift_f2_slot_ops(void);

/*
 * Makes slot hold row of the granule: a reference to gb is kept until the slot is cleared or overwritten. gm: the patches of column deltas the reader's snapshot sees on the granule
 * (a granule map of map, which the slot holds a reference of while it reads them; NULL for none, and for a granule whose decoded columns already hold them: the sequential scan's). A slot of
 * another kind is filled with every attribute now.
 */
extern void sift_f2_slot_store(TupleTableSlot *slot, Relation rel, SiftF2GranuleBuf *gb, uint16 row, SiftColDeltaPartMap *map, const SiftColDeltaGranMap *gm, MemoryContext rowcx);

#endif							/* SIFT_TBL_F2_SLOT_H */
