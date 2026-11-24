/*
 * agg/v6/v6_proj_sort_ios.c - the row of SiftProjSortScan from the cells of the projection (agg/v6/v6_proj_sort_ios.h).
 */
#include "postgres.h"

#include "access/visibilitymap.h"
#include "catalog/pg_type_d.h"

#include "agg/v6/v6_proj_sort_ios.h"

bool
sift_v6_sort_ios_type(Oid type)
{
	switch (type)
	{
		case BOOLOID:
		case INT2OID:
		case INT4OID:
		case INT8OID:
		case DATEOID:
		case TIMESTAMPOID:
		case TIMESTAMPTZOID:
			return true;
		default:
			return false;
	}
}

static Datum
cell_datum(Oid type, int64 v)
{
	switch (type)
	{
		case BOOLOID:
			return BoolGetDatum(v != 0);
		case INT2OID:
			return Int16GetDatum((int16) v);
		case INT4OID:
		case DATEOID:
			return Int32GetDatum((int32) v);
		default:
			return Int64GetDatum(v);
	}
}

bool
sift_v6_sort_ios_fill(TupleTableSlot *slot, const SortIosCols *cols, const SiftCell *row)
{
	const TupleDesc td = slot->tts_tupleDescriptor;

	ExecClearTuple(slot);
	memset(slot->tts_isnull, true, td->natts * sizeof(bool));
	for (int i = 0; i < cols->n; i++)
	{
		const SiftCell *cell = &row[cols->leaf[i]];
		const int	at = cols->att[i] - 1;

		if (cell->tag != 0)
			return false;
		slot->tts_isnull[at] = cell->isnull;
		slot->tts_values[at] = cell->isnull ? (Datum) 0 : cell_datum(TupleDescAttr(td, at)->atttypid, cell->i);
	}
	ExecStoreVirtualTuple(slot);
	return true;
}

bool
sift_v6_sort_ios_visible(Relation rel, uint64 addr, Buffer *vmbuf)
{
	return (visibilitymap_get_status(rel, (BlockNumber) (addr >> 16), vmbuf) & VISIBILITYMAP_ALL_VISIBLE) != 0;
}
