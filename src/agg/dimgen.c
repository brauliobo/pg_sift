/*
 * agg/dimgen.c - the state of a dimension a map was read at, and whether the map is still the dimension.
 *
 * A dimension map is valid for a statement when the dimension holds the same rows for it as when the map was read. The state is the one the cache of
 * the rows of a joined table uses (agg/lookcache.c): the LSN of every page of the heap, all of them visible to every transaction, taken before and after the
 * read; a write moves an LSN, so any change of the dimension, committed or not, is a different state and the map is read again.
 */
#include "postgres.h"

#include "agg/agg_int.h"

struct DimStamp
{
	LookFinger	finger;
};

/* the state of the dimension now; NULL when no map can be vouched for (a page not all-visible, a table too large for the budget, a relation the cache does not take) */
DimStamp *
dimgen_take(Relation rel)
{
	LookFinger *f = look_finger(rel, lookshm_bytes(LOOK_DIM));
	DimStamp   *st;

	if (f == NULL)
		return NULL;
	st = palloc(sizeof(DimStamp));
	st->finger = *f;
	return st;
}

bool
dimgen_same(const DimStamp *a, const DimStamp *b)
{
	return look_finger_same(&a->finger, &b->finger);
}

const LookFinger *
dimgen_finger(const DimStamp *st)
{
	return &st->finger;
}
