/*
 * agg/lookvalid.c - what a kept copy of a joined table stands for (the rows of a join, the classes of a dimension and its map), and whether the table still is it.
 *
 * A copy is valid for a statement when the table holds the same rows for it as when the copy was read. Two rules say so, taken before the read (lookvalid_take) and
 * asked again after it (lookvalid_holds), the same for every copy:
 *
 * - the parent index keeps a journal of its changed keys (col/conn.h, made when a child index is connected to it, on the key the join is by): the copy is
 *   stamped with the horizon of the snapshot it was read under, and a statement reads it as it is when the journal vouches for its snapshot, no row was inserted
 *   into the parent since the horizon and no key was changed in it. A write to the table makes the stamp unreadable for the statements after it, which
 *   read the table again and keep a copy of their own under a later horizon; no vacuum and no limit on the pages is involved.
 * - otherwise the state is the one the cache of the rows of a joined table uses (agg/lookcache.c): the LSN of every page of the heap, all of them visible to
 *   every transaction, taken before and after the read; a write moves an LSN, so any change of the table, committed or not, is a different state.
 */
#include "postgres.h"

#include "col/am.h"
#include "col/conn.h"

#include "agg/agg_int.h"

struct LookValid
{
	LookFinger	finger;
	XLogRecPtr	stamp[2];
	bool		journal;
	Size		budget;			/* the bytes of the store the copy is for: the pages are not read of a table the store cannot take */
};

/* does the parent index keep a journal of the key the join is by? (index NULL: the table has no connected index) */
bool
lookvalid_journal(Relation index, AttrNumber keyatt)
{
	return index != NULL && sift_conn_journal_on(index) && index->rd_index->indkey.values[sift_col_rel(index)->rowkey_attno - 1] == keyatt;
}

/*
 * The state of the table now, taken before its rows are read under snap: the stamp of the journal of index when it has one that vouches for the snapshot,
 * else the pages of the heap, for a store of budget bytes. NULL when no copy can be vouched for (a page not all-visible, a table too large for the budget, a relation
 * the cache does not take).
 */
LookValid *
lookvalid_take(Relation rel, Relation index, AttrNumber keyatt, Snapshot snap, Size budget)
{
	LookValid  *st = palloc0(sizeof(LookValid));
	SiftConnStamp cs;
	LookFinger *f;

	st->budget = budget;
	if (lookvalid_journal(index, keyatt) && sift_conn_take(rel, index, snap, &cs, NULL) == SIFT_CONN_OK)
	{
		st->journal = true;
		st->stamp[0] = cs.lineage;
		st->stamp[1] = cs.xmin;
		st->finger = (LookFinger) {.rel = index->rd_locator,.nblocks = 2,.lsn = st->stamp,.stamp = true};
		return st;
	}
	if ((f = look_finger(rel, budget)) == NULL)
		return NULL;
	st->finger = *f;
	return st;
}

/* may a statement under snap read what the stamp stands for? */
bool
lookvalid_readable(Relation rel, Relation index, Snapshot snap, const LookFinger *stored)
{
	SiftConnStamp cs = {.lineage = stored->lsn[0],.xmin = (TransactionId) stored->lsn[1]};
	SiftConnKeys keys = {0};

	return stored->stamp && sift_conn_vouches(rel, index, &cs, snap) == SIFT_CONN_OK && !sift_conn_inserted_since(rel, index, &cs) &&
		sift_conn_changed(rel, index, &cs, snap, 0, &keys) == SIFT_CONN_OK;
}

/* is the table, after its rows were read, the one the stamp was taken of, and one every later statement may read? */
bool
lookvalid_holds(Relation rel, Relation index, AttrNumber keyatt, Snapshot snap, const LookValid *st)
{
	LookValid  *now;

	if (st->journal)
		return lookvalid_readable(rel, index, snap, &st->finger);
	return (now = lookvalid_take(rel, index, keyatt, snap, st->budget)) != NULL && look_finger_same(&st->finger, &now->finger);
}

bool
lookvalid_stamped(const LookValid *st)
{
	return st->journal;
}

const LookFinger *
lookvalid_finger(const LookValid *st)
{
	return &st->finger;
}
