/*
 * agg/dimgen.c - the state of a dimension a map was read at, and whether the map is still the dimension.
 *
 * A dimension map is valid for a statement when the dimension holds the same rows for it as when the map was read. Two rules say so:
 *
 * - the parent index keeps a journal of its changed keys (col/conn.h, made when a child index is connected to it, on the key the join is by): the map is
 *   stamped with the horizon of the snapshot it was read under, and a statement reads it as it is when the journal vouches for its snapshot, no row was inserted
 *   into the parent since the horizon and no key was changed in it. A write to the dimension makes the stamp unreadable for the statements after it, which
 *   read the table again and keep a map of their own under a later horizon; no vacuum and no limit on the pages is involved.
 * - otherwise the state is the one the cache of the rows of a joined table uses (agg/lookcache.c): the LSN of every page of the heap, all of them visible to
 *   every transaction, taken before and after the read; a write moves an LSN, so any change of the dimension, committed or not, is a different state.
 */
#include "postgres.h"

#include "col/am.h"
#include "col/conn.h"

#include "agg/agg_int.h"

struct DimStamp
{
	LookFinger	finger;
	XLogRecPtr	stamp[2];
	bool		journal;
};

/* does the parent index keep a journal of the key the join is by? */
bool
dimgen_journal(Relation index, AttrNumber keyatt)
{
	return sift_conn_journal_on(index) && index->rd_index->indkey.values[sift_col_rel(index)->rowkey_attno - 1] == keyatt;
}

/*
 * The state of the dimension now, taken before its rows are read under snap: the stamp of the journal of index when it has one that vouches for the snapshot,
 * else the pages of the heap. NULL when no map can be vouched for (a page not all-visible, a table too large for the budget, a relation the cache does not
 * take).
 */
DimStamp *
dimgen_take(Relation rel, Relation index, AttrNumber keyatt, Snapshot snap)
{
	DimStamp   *st = palloc0(sizeof(DimStamp));
	SiftConnStamp cs;
	LookFinger *f;

	if (dimgen_journal(index, keyatt) && sift_conn_take(rel, index, snap, &cs, NULL) == SIFT_CONN_OK)
	{
		st->journal = true;
		st->stamp[0] = cs.lineage;
		st->stamp[1] = cs.xmin;
		st->finger = (LookFinger) {.rel = index->rd_locator,.nblocks = 2,.lsn = st->stamp,.stamp = true};
		return st;
	}
	if ((f = look_finger(rel, lookshm_bytes(LOOK_DIM))) == NULL)
		return NULL;
	st->finger = *f;
	return st;
}

/* may a statement under snap read what the stamp stands for? */
bool
dimgen_readable(Relation rel, Relation index, Snapshot snap, const LookFinger *stored)
{
	SiftConnStamp cs = {.lineage = stored->lsn[0],.xmin = (TransactionId) stored->lsn[1]};
	SiftConnKeys keys = {0};

	return stored->stamp && sift_conn_vouches(rel, index, &cs, snap) == SIFT_CONN_OK && !sift_conn_inserted_since(rel, index, &cs) &&
		sift_conn_changed(rel, index, &cs, snap, 0, &keys) == SIFT_CONN_OK;
}

/* is the dimension, after its rows were read, the one the stamp was taken of, and one every later statement may read? */
bool
dimgen_holds(Relation rel, Relation index, AttrNumber keyatt, Snapshot snap, const DimStamp *st)
{
	DimStamp   *now;

	if (st->journal)
		return dimgen_readable(rel, index, snap, &st->finger);
	return (now = dimgen_take(rel, index, keyatt, snap)) != NULL && look_finger_same(&st->finger, &now->finger);
}

bool
dimgen_stamped(const DimStamp *st)
{
	return st->journal;
}

const LookFinger *
dimgen_finger(const DimStamp *st)
{
	return &st->finger;
}
